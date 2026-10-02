#!/usr/bin/env python3
"""The CAN bus over USB: ties the boards plugged into this computer together (docs/protocols/usb_link.md).

Opens every board's USB serial port, sends the keepalives that switch on each board's USB link, and copies every
CAN frame a board sends to all the others. The boards' console text is printed with the board's name, and lines
you type go to a board's shell.

    python3 tools/can_relay.py                         # every USB serial port found, peer mode
    python3 tools/can_relay.py --frames                # also log the frames
    python3 tools/can_relay.py --gateway center        # the center bridges this computer onto the real CAN bus
    python3 tools/can_relay.py --no-sensors --no-time  # only relay what the boards send

The relay also stands in for the sensor pack and GPS on the bench: it sends a simulated drive (the firmware's 60 s
demo cycle: idle, pull away, cruise, pass, stop) as sensor frames 0x100-0x104 at 20 Hz, and this computer's clock as
the time frame 0x220 once a second. The screens use them only while their own sensor UART and GPS are silent. Both
are off with --gateway (a real bus has the real ones).

It also stands in for the slot cards that are not on the bench, each speaking the channel protocol as its firmware
will (docs/protocols/can_channels.md): heartbeat, NET ANNOUNCE / QUERY / IDENTIFY / RESTART, and

    radio   AM/FM radio card, slot 2, node 0x15 (tools/radio_sim.py): its UI page, signal and SNR, RAIL, MEDIA
    audio   audio card, slot 1, node 0x14 (tools/cards_sim.py, the rest too): AUDIO_CMD STATUS, sounds, volume,
            media keys, RET arbitration, Source / Volume / Fader / Balance page
    gps     GPS card, slot 3, node 0x16: speed (following the simulated drive), heading, altitude, satellites
    lte     LTE gateway, slot 4, node 0x17: GATEWAY STATUS, ONLINE place, weather, time zone, a message
    voice   AI voice card, slot 5, node 0x18: asks for RET and speaks on ':voice say'
    trans   transmission-temperature card, slot 6, node 0x19: SENSOR, its own ALERTs, an Offset item

All on in every mode, gateway too, since none of these cards is built yet. The cards hear each other's frames as well
as the boards'. --no-sim audio,gps turns cards off (--no-radio is --no-sim radio), --radio-slot / --trans-slot move
those two.

Typing:
    center help          a command to one board (center, left, right, console, bridge, a node like 0x14, or a port)
    all can              to every board
    :frames on|off       frame log;  :filter 0x215 0x404  (no ids = all)
    :sensors on|off      the simulated drive;  :time on|off   the clock
    :sim list            the simulated cards;  :sim on|off CARD  (off: the card goes silent; on: it boots again)
    :radio               status | on | off | source on|off | tune 98.3 | seek up|down
    :audio               status | source bt|radio | volume 0-100 | mute | play SOUND [PRIORITY] [REPEAT] | stop |
                         media play|next|prev
    :gps                 status | fix on|off
    :lte                 status | message [text] | refresh | offline | online
    :voice               status | say [text]
    :trans               status | temp 120 (deg C: caution at 110, warning at 125) | mode own|center
    :ports               the boards and their links
    :quit                (or Ctrl-C) the boards fall back to plain serial within 3 s

    python3 tools/can_relay.py --test   # the relay's records and the simulated cards together, no hardware

Never toggles DTR/RTS: on the Waveshare P4 boards the CH343's auto-reset circuit reboots the ESP32.
"""
from __future__ import annotations

import argparse
import binascii
import glob
import math
import struct
import sys
import threading
import time

from collections import deque

from cards_sim import AudioCard, GpsCard, LteGateway, TransTempCard, VoiceCard
from chan_sim import CH_NET, SimCard
from radio_sim import RadioSim

try:
    import serial  # type: ignore
    from serial.tools import list_ports  # type: ignore
except ImportError:  # only --test runs without it
    serial = list_ports = None  # type: ignore

DEFAULT_BAUD = 115200
KEEPALIVE_S = 1.0
TIMEOUT_S = 3.0
VERSION = 1
NODE_NAMES = {0x01: "center", 0x02: "left", 0x03: "right", 0x13: "console"}
TYPE_BRIDGE = 1
CHANNEL_NAMES = {0x020: "alert", 0x040: "input", 0x060: "system", 0x120: "sensor", 0x240: "audio-cmd", 0x260: "ui",
                 0x320: "bulk", 0x360: "online", 0x3C0: "net", 0x3E0: "log"}
ID_NAMES = {
    0x100: "speed", 0x101: "analog", 0x102: "status", 0x103: "odo", 0x104: "econ", 0x210: "cluster",
    0x211: "art-req", 0x212: "setting", 0x213: "center-st", 0x215: "phone-cmd", 0x216: "menu-in",
    0x217: "menu-st", 0x218: "menu-main", 0x219: "menu-sub", 0x220: "time",
}

print_lock = threading.Lock()


def id_name(can_id: int) -> str:
    if 0x700 <= can_id < 0x780:
        return "heartbeat"
    return ID_NAMES.get(can_id) or CHANNEL_NAMES.get(can_id & ~0x1F, "")


def out(line: str) -> None:
    with print_lock:
        print(line, flush=True)


# ---- records: 0x00, COBS(payload + CRC-16/CCITT-FALSE little-endian), 0x00

def crc16(data: bytes) -> int:
    return binascii.crc_hqx(data, 0xFFFF)


def wrap(payload: bytes) -> bytes:
    raw = payload + struct.pack("<H", crc16(payload))
    enc = bytearray()
    block = bytearray()
    for b in raw:
        if b == 0:
            enc.append(len(block) + 1)
            enc += block
            block = bytearray()
        else:
            block.append(b)
    enc.append(len(block) + 1)
    enc += block
    return b"\x00" + bytes(enc) + b"\x00"


def unwrap(enc: bytes) -> bytes | None:
    dec = bytearray()
    i = 0
    while i < len(enc):
        code = enc[i]
        i += 1
        if code == 0 or i + code - 1 > len(enc):
            return None
        dec += enc[i:i + code - 1]
        i += code - 1
        if code < 0xFF and i < len(enc):
            dec.append(0)
    if len(dec) < 3 or crc16(bytes(dec[:-2])) != struct.unpack("<H", dec[-2:])[0]:
        return None
    return bytes(dec[:-2])


class Parser:
    """Same rules as ElcUsbParser in elc_usb_link.cpp."""

    MAX = 22

    def __init__(self) -> None:
        self.inside = False
        self.buf = bytearray()
        self.bad = 0

    def feed(self, data: bytes):
        """Yields ("text", bytes) and ("rec", payload)."""
        text = bytearray()
        for b in data:
            if not self.inside:
                if b == 0:
                    if text:
                        yield "text", bytes(text)
                        text = bytearray()
                    self.inside = True
                    self.buf = bytearray()
                else:
                    text.append(b)
                continue
            if b != 0:
                if len(self.buf) < self.MAX:
                    self.buf.append(b)
                else:
                    self.bad += 1
                    self.inside = False
                    self.buf = bytearray()
                continue
            if not self.buf:
                continue
            p = unwrap(bytes(self.buf))
            self.buf = bytearray()
            if p is None:
                self.bad += 1  # stay inside: this 0x00 may open the next record
                continue
            self.inside = False
            yield "rec", p
        if text:
            yield "text", bytes(text)


def frame_record(can_id: int, data: bytes) -> bytes:
    return wrap(struct.pack("<BHB", ord("F"), can_id, len(data)) + data)


def keepalive_record(gateway: bool, baud: int) -> bytes:
    return wrap(struct.pack("<BBBI", ord("K"), VERSION, 1 if gateway else 0, baud))


# ---- what the relay sends itself: a simulated drive and the time

def clampf(v: float, a: float, b: float) -> float:
    return a if v < a else (b if v > b else v)


def therm_adc_for_f(f: float) -> int:
    """Inverse of the 100k NTC / B3950 conversion: temperature F -> ADC count (demo_drive.cpp)."""
    t_k = (f - 32.0) * 5.0 / 9.0 + 273.15
    r = 100000.0 * math.exp(3950.0 * (1.0 / t_k - 1.0 / 298.15))
    return int(clampf(1023.0 / (1.0 + r / 100000.0), 1.0, 1022.0))


class DemoDrive:
    """The firmware's DemoDrive (esp32_p4_common/src/demo_drive.cpp), frame for frame."""

    def __init__(self, odometer_tenths: int = 966486) -> None:
        self.t0 = time.monotonic()
        self.last = self.t0
        self.odo = odometer_tenths
        self.odo_frac = 0.0
        self.mph = 0.0
        self.coolant = 150.0
        self.fuel = 64.0
        self.mpg_avg = 17.5
        self.seq = 0

    @staticmethod
    def target_mph(t: float) -> float:
        if t < 4:
            return 0.0
        if t < 16:
            return 55.0 * (t - 4) / 12
        if t < 36:
            return 55.0 + 3.0 * math.sin((t - 16) * 0.5)
        if t < 42:
            return 55.0 + 15.0 * (t - 36) / 6
        if t < 54:
            return 70.0 * (1 - (t - 42) / 12)
        return 0.0

    def frames(self) -> list[tuple[int, bytes]]:
        now = time.monotonic()
        dt = clampf(now - self.last, 0.0, 0.25)
        self.last = now
        t = math.fmod(now - self.t0, 60.0)
        target = self.target_mph(t)
        self.mph += (target - self.mph) * (1 - math.exp(-2 * dt))
        if self.mph < 0.2:
            self.mph = 0.0
        gear = 1 if self.mph < 17 else (2 if self.mph < 38 else 3)
        ratio = [0.0, 118.0, 64.0, 38.0]
        accel = target - self.mph
        rpm = clampf(max(720.0 + max(accel, 0.0) * 60.0, self.mph * ratio[gear]), 0.0, 5800.0)
        self.coolant += (195.0 - self.coolant) * (1 - math.exp(-0.05 * dt))
        self.fuel = max(8.0, self.fuel - dt * 0.02)
        mpg_inst = clampf(26.0 - max(accel, 0.0) * 1.5 - (max(self.mph - 60.0, 0.0) * 0.3), 6.0, 32.0) if self.mph > 3 else 0.0
        if self.mph > 3:
            self.mpg_avg += (mpg_inst - self.mpg_avg) * (1 - math.exp(-0.02 * dt))
        self.odo_frac += self.mph * dt / 3600.0
        while self.odo_frac >= 0.1:
            self.odo_frac -= 0.1
            self.odo += 1
        self.seq = (self.seq + 1) & 0xFF
        pot = int(self.fuel / 100 * 1023)
        therm = therm_adc_for_f(self.coolant)
        press = int(clampf((14.0 + rpm / 95.0) / 100 * 1023, 0, 1023))
        volt = 720  # charging
        ambient = therm_adc_for_f(72.0)
        wheel = int(self.mph / 2.236936 / 2.1 * 60 + 0.5)
        dte = int(self.fuel / 100 * 20 * self.mpg_avg * 10)
        # elc_can_encode_sensor (elc_can_codec.cpp), little-endian
        return [
            (0x100, struct.pack("<II", wheel, int(rpm))),
            (0x101, struct.pack("<HHHH", pot, therm, press, volt)),
            (0x102, struct.pack("<HBHB", ambient, 0x03, 0, self.seq)),  # flags: ALT OK, IGN ON
            (0x103, struct.pack("<II", self.odo, dte)),
            (0x104, struct.pack("<HHB", int(mpg_inst * 10), int(self.mpg_avg * 10), 1)),
        ]


def time_frame() -> tuple[int, bytes]:
    """0x220: UTC s u32, local offset min s16, flags (bit0 valid), reserved."""
    now = time.time()
    offset_min = time.localtime(now).tm_gmtoff // 60  # DST included
    return 0x220, struct.pack("<IhBB", int(now), offset_min, 0x01, 0)


# ---- one board

class Board:
    def __init__(self, relay: "Relay", port: str, fast_baud: int, gateway: bool) -> None:
        self.relay = relay
        self.port = port
        self.fast_baud = fast_baud  # 0: native USB, no baud rate
        self.gateway = gateway
        self.ser = serial.Serial(port, DEFAULT_BAUD, timeout=0.05)
        self.wlock = threading.Lock()
        self.parser = Parser()
        self.node: int | None = None
        self.type: int | None = None
        self.can = False
        self.baud = DEFAULT_BAUD
        self.last_status = 0.0
        self.up = 0
        self.down = 0
        self.line = bytearray()
        self.alive = True

    @property
    def name(self) -> str:
        if self.node is None:
            return self.port.split("/")[-1]
        if self.type == TYPE_BRIDGE:
            return "bridge" if self.relay.count_bridges() <= 1 else f"bridge{self.node:02x}"
        return NODE_NAMES.get(self.node, f"node{self.node:02x}")

    @property
    def linked(self) -> bool:
        return time.time() - self.last_status < TIMEOUT_S

    def write(self, data: bytes) -> None:
        with self.wlock:
            try:
                self.ser.write(data)
            except (serial.SerialException, OSError):
                self.alive = False

    def keepalive(self) -> None:
        # Not heard for a while at a fast rate: the board has gone back to the default (reset, or it timed out).
        if self.baud != DEFAULT_BAUD and not self.linked:
            self.set_baud(DEFAULT_BAUD)
        self.write(keepalive_record(self.gateway, self.fast_baud))

    def set_baud(self, baud: int) -> None:
        with self.wlock:
            try:
                self.ser.baudrate = baud
            except (serial.SerialException, OSError):
                self.alive = False
        self.baud = baud

    def status(self, p: bytes) -> None:
        if len(p) != 11:
            return
        _, ver, node, typ, mode, baud, can, fw = struct.unpack("<BBBBBIBB", p)
        first = not self.linked
        self.node = None if node == 0xFF else node
        self.type = typ
        self.can = bool(can)
        self.last_status = time.time()
        if baud and baud != self.baud:
            time.sleep(0.01)  # the board switches after this record has gone out
            self.set_baud(baud)
        if first:
            out(f"[relay] {self.port}: {self.name} linked ({'gateway' if mode else 'peer'}, "
                f"{self.baud if baud else 'native USB'}, CAN {'up' if can else 'off'}, fw {fw})")

    def reopen(self) -> bool:
        """The port vanished (a reset re-enumerates USB): open it again once it is back, at the default rate."""
        try:
            self.ser.close()
        except (serial.SerialException, OSError):
            pass
        try:
            ser = serial.Serial(self.port, DEFAULT_BAUD, timeout=0.05)
        except (serial.SerialException, OSError):
            return False
        with self.wlock:
            self.ser = ser
            self.parser = Parser()
            self.baud = DEFAULT_BAUD
            self.last_status = 0.0
            self.line = bytearray()
            self.alive = True
        out(f"[relay] {self.port}: back, linking again")
        return True

    def run(self) -> None:
        while self.relay.running:
            if not self.alive:
                if not self.reopen():
                    time.sleep(0.5)
                continue
            try:
                data = self.ser.read(4096)
            except (serial.SerialException, OSError):
                out(f"[relay] {self.port}: gone (waiting for it to come back)")
                self.alive = False
                continue
            if not data:
                continue
            for kind, val in self.parser.feed(data):
                if kind == "text":
                    self.text(val)
                    continue
                if val[:1] == b"S":
                    self.status(val)
                elif val[:1] == b"F" and len(val) >= 4 and len(val) == 4 + val[3]:
                    can_id = val[1] | (val[2] << 8)
                    payload = val[4:]
                    self.up += 1
                    if 0x700 <= can_id < 0x780 and self.node is None:
                        self.node = can_id - 0x700  # a heartbeat before the first status
                    self.relay.frame(self, can_id, payload)

    def text(self, data: bytes) -> None:
        for b in data:
            if b == 0x0A:
                s = self.line.decode("utf-8", "replace").rstrip("\r")
                self.line = bytearray()
                out(f"[{self.name}] {s}")
            elif len(self.line) < 2000:
                self.line.append(b)


# ---- all of them

class Relay:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.running = True
        self.boards: list[Board] = []
        self.show_frames = args.frames
        self.filter: set[int] = set()
        self.sensors = not args.no_sensors and not args.gateway
        self.clock = not args.no_time and not args.gateway
        self.drive = DemoDrive()
        self.cards: dict[str, SimCard] = {}
        self.sim_queue: deque[tuple[str, int, bytes]] = deque()
        off = set(getattr(args, "no_sim", None) or ()) | ({"radio"} if args.no_radio else set())
        for name in SIM_CARDS:
            if name not in off:
                self.cards[name] = self.make_card(name)
        nodes: dict[int, str] = {}
        for name, c in self.cards.items():
            if c.node in nodes:
                out(f"[relay] simulated {name} and {nodes[c.node]} both at node 0x{c.node:02x} "
                    f"(--no-sim one of them, or move it)")
            nodes[c.node] = name

    # ---- the simulated cards

    def make_card(self, name: str) -> SimCard:
        send = lambda i, d: self.sim_send(name, i, d)  # noqa: E731
        log = lambda t: out(f"[{name}] {t}")  # noqa: E731
        if name == "radio":
            return RadioSim(send, log, slot=self.args.radio_slot)
        if name == "audio":
            return AudioCard(send, log)
        if name == "gps":
            return GpsCard(send, log, speed_kmh=self.drive_kmh)
        if name == "lte":
            return LteGateway(send, log, speed_kmh=self.drive_kmh)
        if name == "voice":
            return VoiceCard(send, log)
        return TransTempCard(send, log, slot=getattr(self.args, "trans_slot", 6))

    @property
    def radio(self) -> RadioSim | None:
        return self.cards.get("radio")  # type: ignore

    def drive_kmh(self) -> float:
        """The simulated drive's speed, for the GPS card and the gateway (the drive's cycle even with :sensors off)."""
        if self.sensors:
            return self.drive.mph * 1.609344
        return DemoDrive.target_mph(math.fmod(time.monotonic() - self.drive.t0, 60.0)) * 1.609344

    def sim_send(self, name: str, can_id: int, data: bytes) -> None:
        """A frame from a simulated card: to the boards now, to the other simulated cards from the simulate loop."""
        self.inject(can_id, data, name)
        self.sim_queue.append((name, can_id, data))

    def deliver_sim(self) -> None:
        # Not from inside sim_send: the sender holds its own lock there, and the receiver may answer at once.
        for _ in range(10000):
            try:
                name, can_id, data = self.sim_queue.popleft()
            except IndexError:
                return
            for other, card in list(self.cards.items()):
                if other != name:
                    card.on_frame(can_id, data)

    def sim_switch(self, on: bool, name: str) -> None:
        if name not in SIM_CARDS:
            out(f"[relay] no simulated card '{name}' ({', '.join(SIM_CARDS)})")
            return
        if on == (name in self.cards):
            out(f"[relay] {name} is already {'on' if on else 'off'}")
            return
        if on:
            card = self.make_card(name)
            with card.lock:
                card.boot()
                # Booted after the others: ask every node for its ANNOUNCE, so it finds the audio card and the radio.
                card.transfer(CH_NET, bytes([0x81, 0x00, 0x01]))
            self.cards = {n: (card if n == name else self.cards[n]) for n in SIM_CARDS
                          if n in self.cards or n == name}
        else:
            self.cards = {n: c for n, c in self.cards.items() if n != name}
            out(f"[relay] {name} off: silent from now (no heartbeat)")

    def count_bridges(self) -> int:
        return sum(1 for b in self.boards if b.type == TYPE_BRIDGE)

    def frame(self, src: Board, can_id: int, data: bytes) -> None:
        rec = frame_record(can_id, data)
        for b in self.boards:
            if b is not src and b.alive and b.linked:
                b.write(rec)
                b.down += 1
        for card in list(self.cards.values()):
            card.on_frame(can_id, data)
        if self.show_frames and (not self.filter or can_id in self.filter):
            out(f"  {src.name:>8} 0x{can_id:03x} {id_name(can_id):<10} {data.hex(' ')}")

    def find(self, who: str) -> list[Board]:
        if who == "all":
            return list(self.boards)
        hits = [b for b in self.boards if who in (b.name, b.port, b.port.split('/')[-1])]
        if not hits and who.startswith("0x"):
            try:
                n = int(who, 16)
                hits = [b for b in self.boards if b.node == n]
            except ValueError:
                pass
        return hits

    def command(self, line: str) -> None:
        line = line.strip()
        if not line:
            return
        if line.startswith(":"):
            words = line[1:].split()
            cmd = words[0] if words else ""
            if cmd == "quit":
                self.running = False
            elif cmd == "frames":
                self.show_frames = len(words) < 2 or words[1] == "on"
                out(f"[relay] frame log {'on' if self.show_frames else 'off'}")
            elif cmd == "filter":
                self.filter = {int(w, 16) for w in words[1:]}
                out(f"[relay] filter {' '.join(hex(i) for i in sorted(self.filter)) or 'off'}")
            elif cmd in ("sensors", "time"):
                on = len(words) < 2 or words[1] == "on"
                if cmd == "sensors":
                    self.sensors = on
                else:
                    self.clock = on
                out(f"[relay] {'simulated drive' if cmd == 'sensors' else 'time'} {'on' if on else 'off'}")
            elif cmd == "ports":
                for b in self.boards:
                    out(f"[relay] {b.name:>8} {b.port} {'linked' if b.linked else 'not linked'} "
                        f"{'gateway' if b.gateway else 'peer'} baud {b.baud} CAN {'up' if b.can else 'off'} "
                        f"frames up {b.up} down {b.down} bad {b.parser.bad}")
            elif cmd == "sim":
                self.sim_command(words[1:])
            elif cmd in SIM_CARDS:
                self.card_command(cmd, words[1:])
            else:
                out("[relay] :frames on|off, :filter <ids>, :sensors on|off, :time on|off, :sim list|on|off CARD, "
                    f":{' ..., :'.join(SIM_CARDS)} ..., :ports, :quit")
            return
        who, _, rest = line.partition(" ")
        targets = self.find(who)
        if not targets:
            if len(self.boards) == 1:
                targets, rest = self.boards, line
            else:
                out(f"[relay] no board '{who}' (try :ports)")
                return
        for b in targets:
            b.write(rest.encode() + b"\n")

    def sim_command(self, words: list[str]) -> None:
        if words[:1] in ([], ["list"]):
            for name in SIM_CARDS:
                c = self.cards.get(name)
                if c:
                    with c.lock:
                        out(f"[relay] {name:>6} on   {c.status()}")
                else:
                    out(f"[relay] {name:>6} off")
        elif words[0] in ("on", "off") and len(words) > 1:
            for name in words[1].split(","):
                self.sim_switch(words[0] == "on", name)
        else:
            out("[relay] :sim list | :sim on CARD | :sim off CARD")

    def card_command(self, name: str, words: list[str]) -> None:
        card = self.cards.get(name)
        if card is None:
            out(f"[relay] the simulated {name} is off (:sim on {name})")
            return
        if name == "radio" and words[:1] == ["source"] and "audio" in self.cards:
            # The source is the audio card's choice: set it there, and it tells the radio.
            audio = self.cards["audio"]
            with audio.lock:
                audio.command(["source", "bt" if words[1:2] == ["off"] else "radio"])
            return
        with card.lock:
            text = card.command(words)
        if text is None:
            out(f"[relay] :{name} {card.HELP}")
        elif text:
            out(f"[{name}] {text}")

    def inject(self, can_id: int, data: bytes, who: str = "relay") -> None:
        """A frame from the relay itself, to every linked board."""
        rec = frame_record(can_id, data)
        for b in self.boards:
            if b.alive and b.linked:
                b.write(rec)
                b.down += 1
        if self.show_frames and (not self.filter or can_id in self.filter):
            out(f"  {who:>8} 0x{can_id:03x} {id_name(can_id):<10} {data.hex(' ')}")

    def simulate(self) -> None:
        next_time = 0.0
        for card in list(self.cards.values()):
            with card.lock:
                card.boot()
        while self.running:
            now = time.monotonic()
            self.deliver_sim()
            for card in list(self.cards.values()):
                card.tick()
            self.deliver_sim()
            if self.sensors:
                for can_id, data in self.drive.frames():
                    self.inject(can_id, data)
            if self.clock and now >= next_time:
                next_time = now + 1.0
                self.inject(*time_frame())
            time.sleep(0.05)

    def keepalives(self) -> None:
        while self.running:
            for b in list(self.boards):
                if b.alive:
                    b.keepalive()
            if not self.args.ports:
                self.add_new_ports()
            time.sleep(KEEPALIVE_S)

    def add_new_ports(self) -> None:
        """A board plugged in after the start (or one whose port came back under a new name)."""
        known = {b.port for b in self.boards}
        for p in find_ports():
            if p in known:
                continue
            try:
                b = Board(self, p, fast_baud_for(p, self.args.baud), False)
            except (serial.SerialException, OSError):
                continue
            self.boards.append(b)
            threading.Thread(target=b.run, daemon=True).start()
            out(f"[relay] opened {p}")


SIM_CARDS = ("radio", "audio", "gps", "lte", "voice", "trans")


def sim_list(text: str) -> list[str]:
    names = [n for n in text.split(",") if n]
    bad = [n for n in names if n not in SIM_CARDS]
    if bad:
        raise argparse.ArgumentTypeError(f"no simulated card {', '.join(bad)} (cards: {', '.join(SIM_CARDS)})")
    return names


def find_ports() -> list[str]:
    return sorted(set(
        glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/cu.usbserial*") + glob.glob("/dev/cu.wchusbserial*") +
        glob.glob("/dev/cu.SLAB_USBtoUART*") + glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*")))


def fast_baud_for(port: str, requested: int) -> int:
    for p in list_ports.comports():
        if p.device == port:
            if p.vid == 0x303A:  # Espressif native USB (the console): no baud rate
                return 0
            if p.vid == 0x10C4:  # CP210x (DevKitC): stay within what every CP2102 does
                return min(requested, 921600)
    return requested


# ---- self-test (no hardware): the records, and the simulated cards running together

class _TestBoard:
    """Stands in for a Board: collects what the relay writes to it."""

    def __init__(self) -> None:
        self.alive, self.linked, self.down, self.name = True, True, 0, "test"
        self.parser = Parser()
        self.frames: list[tuple[int, bytes]] = []
        self.lock = threading.Lock()

    def write(self, data: bytes) -> None:
        with self.lock:
            for kind, p in self.parser.feed(data):
                if kind == "rec" and p[:1] == b"F":
                    self.frames.append((p[1] | p[2] << 8, p[4:4 + p[3]]))

    def transfers(self, base: int, src: int, op: int | None = None) -> list[bytes]:
        from chan_sim import Reassembler
        rx = Reassembler()
        with self.lock:
            frames = list(self.frames)
        got = []
        for i, d in frames:
            if i == base + src:
                p = rx.feed(i, d, 0.0)
                rx.done.clear()
                if p and (op is None or p[0] == op):
                    got.append(p)
        return got


def _test() -> None:
    from chan_sim import CH_ALERT, CH_AUDIO_CMD, CH_ONLINE, CH_SENSOR, CH_UI, transfer_frames
    p = struct.pack("<BHB", ord("F"), 0x215, 3) + b"\x00\x01\x02"
    assert unwrap(wrap(p)[1:-1]) == p and list(Parser().feed(b"hi" + wrap(p))) == [("text", b"hi"), ("rec", p)]

    args = argparse.Namespace(frames=False, no_sensors=False, no_time=False, gateway=None, no_radio=False,
                              no_sim=[], radio_slot=2, trans_slot=6, ports=["test"], baud=2000000)
    relay = Relay(args)
    assert list(relay.cards) == list(SIM_CARDS)
    board = _TestBoard()
    relay.boards.append(board)  # type: ignore
    th = threading.Thread(target=relay.simulate, daemon=True)
    th.start()

    def wait(cond, s: float, what: str) -> None:
        end = time.monotonic() + s
        while time.monotonic() < end:
            if cond():
                return
            time.sleep(0.05)
        raise AssertionError(what)

    radio, audio, voice, trans = relay.cards["radio"], relay.cards["audio"], relay.cards["voice"], relay.cards["trans"]
    wait(lambda: all((0x700 + n, ) in {(i,) for i, _ in board.frames} for n in (0x14, 0x15, 0x16, 0x17, 0x18, 0x19)),
         2.0, "a heartbeat from every card")
    wait(lambda: radio.sink_node == 0x14 and voice.sink_node == 0x14, 1.0, "the cards hear each other's ANNOUNCE")
    assert audio.find(9) == 0x15
    # the audio card picks the radio as its source: the radio asks for RET and gets it
    relay.command(":audio source radio")
    wait(lambda: radio.rail == 2 and audio.holder == 0x15, 1.0, "the radio holds RET")
    # the voice card speaks: the radio's grant is withdrawn, and it gets RET back after the voice card releases it
    relay.command(":voice say Turn left")
    wait(lambda: voice.state == "speaking" and audio.holder == 0x18 and radio.rail == 0, 1.0, "the voice card speaks")
    wait(lambda: voice.state == "idle" and audio.holder == 0x15 and radio.rail == 2, 6.5, "RET back to the radio")
    # a SET from the center reaches the audio card; RESULT and VALUE go to the board
    for f in transfer_frames(struct.pack("<BBBi", 0x81, 0x14, 1, 64), 9):
        relay.frame(board, CH_UI + 0x01, f)  # type: ignore
    wait(lambda: any(r == bytes([0x04, 1, 0, 0x01]) for r in board.transfers(CH_UI, 0x14, 0x04)), 1.0, "RESULT")
    assert audio.volume == 64
    # next on the console with Source = Radio: the audio card forwards MEDIA, the radio seeks
    for f in transfer_frames(bytes([0x01, 17, 1, 1]), 1):
        relay.frame(board, 0x040 + 0x13, f)  # type: ignore
    wait(lambda: any(r == bytes([0x84, 0x15, 4, 0]) for r in board.transfers(CH_AUDIO_CMD, 0x14, 0x84)), 1.0, "MEDIA")
    wait(lambda: any(b"seek" in r for r in board.transfers(0x3E0, 0x15)), 2.0, "the radio seeks")
    # the trans temp card past its warning threshold
    with trans.lock:
        trans.temp = 126.0
        trans.command(["temp", "126"])
    wait(lambda: any(r[:2] == bytes([0x02, 2]) for r in board.transfers(CH_ALERT, 0x19, 0x02)), 1.5, "ALERT STATE")
    assert board.transfers(CH_SENSOR, 0x16, 0x01) and board.transfers(CH_ONLINE, 0x17, 0x01)
    assert board.transfers(CH_NET, 0x17, 0x11)
    # :sim off: the card goes silent; on: it boots again
    relay.command(":sim off gps")
    assert "gps" not in relay.cards
    relay.command(":sim on gps")
    assert list(relay.cards)[2] == "gps"
    relay.command(":sim list")
    relay.running = False
    th.join(1.0)
    print("can_relay: all tests passed")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ports", nargs="*", help="serial ports (default: every USB serial port)")
    ap.add_argument("--baud", type=int, default=2000000, help="fast rate for USB-UART boards (default 2000000)")
    ap.add_argument("--gateway", help="this board (name or port) bridges onto its real CAN bus")
    ap.add_argument("--frames", action="store_true", help="log every frame")
    ap.add_argument("--no-sensors", action="store_true", help="don't send the simulated drive")
    ap.add_argument("--no-time", action="store_true", help="don't send this computer's clock")
    ap.add_argument("--no-radio", action="store_true", help="don't simulate the AM/FM radio card (= --no-sim radio)")
    ap.add_argument("--no-sim", type=sim_list, default=[], metavar="CARD[,CARD]",
                    help=f"simulated cards to leave off: {', '.join(SIM_CARDS)}")
    ap.add_argument("--radio-slot", type=int, default=2, choices=range(2, 7), metavar="2-6",
                    help="slot of the simulated radio card (node 0x14 + slot - 1, default 2)")
    ap.add_argument("--trans-slot", type=int, default=6, choices=range(1, 7), metavar="1-6",
                    help="slot of the simulated transmission-temperature card (default 6; the spec's example has "
                         "it in slot 1, which needs --no-sim audio)")
    ap.add_argument("--test", action="store_true", help="run the self-test (no hardware) and exit")
    args = ap.parse_args()
    if args.test:
        _test()
        return
    if serial is None:
        sys.exit("pyserial is required: pip install pyserial")

    ports = args.ports or find_ports()
    if not ports:
        sys.exit("No USB serial port found")
    relay = Relay(args)
    for p in ports:
        try:
            gw = bool(args.gateway) and args.gateway in (p, p.split("/")[-1])
            relay.boards.append(Board(relay, p, fast_baud_for(p, args.baud), gw))
            out(f"[relay] opened {p}")
        except (serial.SerialException, OSError) as e:
            out(f"[relay] {p}: {e}")
    threads = [threading.Thread(target=b.run, daemon=True) for b in relay.boards]
    threads.append(threading.Thread(target=relay.keepalives, daemon=True))
    threads.append(threading.Thread(target=relay.simulate, daemon=True))
    sims = [w for w, on in (("a simulated drive", relay.sensors), ("the time", relay.clock),
                            (f"simulated cards ({', '.join(relay.cards)})", relay.cards)) if on]
    if sims:
        out(f"[relay] sending {', '.join(sims)}")
    for t in threads:
        t.start()

    # --gateway by name: once the boards have said who they are.
    if args.gateway and not any(b.gateway for b in relay.boards):
        time.sleep(KEEPALIVE_S * 2.5)
        hits = relay.find(args.gateway)
        if len(hits) != 1:
            out(f"[relay] --gateway {args.gateway}: {'no such board' if not hits else 'more than one match'}")
        else:
            hits[0].gateway = True
            out(f"[relay] {hits[0].name} is the gateway")

    try:
        while relay.running:
            line = sys.stdin.readline()
            if not line:
                while relay.running:  # stdin closed (piped): keep relaying
                    time.sleep(1)
                break
            relay.command(line)
    except KeyboardInterrupt:
        pass
    relay.running = False
    time.sleep(0.2)


if __name__ == "__main__":
    main()
