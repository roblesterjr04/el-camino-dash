#!/usr/bin/env python3
"""What every simulated card shares (docs/protocols/can_channels.md): the channel IDs and registries, transfer framing
(tail byte, multi-frame with CRC-16/CCITT-FALSE), reassembly, and a base class for a card that speaks the protocol.

SimCard does what every node does: heartbeat 0x700 + node at 1 Hz (flags bit0), NET ANNOUNCE at boot and on QUERY
(after node x 8 ms), the descriptions QUERY asks for, IDENTIFY and RESTART. It remembers every node's ANNOUNCE and
heartbeat, so a card can find the audio card or the radio the way the firmware does (by node type and product). A card
adds its own channels by overriding the hooks: receive(), heard(), on_rail(), periodic(), sensor_defines(),
alert_defines(), items(), command(), status().

The cards are driven by can_relay.py: `send(can_id, data)` puts one frame on the bus, `log(text)` prints on the
relay, `on_frame(can_id, data)` hands in every frame heard, `tick()` is called every 50 ms. `clock` (default
time.monotonic) can be replaced so the self-tests can run time forward.

    python3 tools/chan_sim.py --test     # framing and the base card, no hardware
"""
from __future__ import annotations

import binascii
import random
import struct
import threading
import time
from typing import Callable

# ---- channels (can_channels.md, ID map): base + node

CH_ALERT, CH_INPUT, CH_SYSTEM, CH_SENSOR = 0x020, 0x040, 0x060, 0x120
CH_AUDIO_CMD, CH_UI, CH_ONLINE, CH_NET, CH_LOG = 0x240, 0x260, 0x360, 0x3C0, 0x3E0
HEARTBEAT = 0x700
CHANNEL_BIT = {name: base >> 5 for name, base in (("ALERT", CH_ALERT), ("INPUT", CH_INPUT), ("SYSTEM", CH_SYSTEM),
                                                  ("SENSOR", CH_SENSOR), ("AUDIO_CMD", CH_AUDIO_CMD), ("UI", CH_UI),
                                                  ("ONLINE", CH_ONLINE), ("NET", CH_NET), ("LOG", CH_LOG))}

# node types and products (Registries)
NODE_TYPE_SENSOR, NODE_TYPE_SINK, NODE_TYPE_GATEWAY, NODE_TYPE_VOICE, NODE_TYPE_RADIO = 3, 5, 7, 8, 9
PRODUCT_NONE, PRODUCT_AUDIO_CARD, PRODUCT_GPS, PRODUCT_LTE, PRODUCT_VOICE, PRODUCT_RADIO = 0, 4, 5, 6, 7, 9
NONE32 = -0x80000000

# NET opcodes
NET_ANNOUNCE, NET_GATEWAY_STATUS, NET_QUERY, NET_IDENTIFY, NET_RESTART, NET_RAIL, NET_GATEWAY_CMD = \
    0x01, 0x11, 0x81, 0x82, 0x83, 0x84, 0x8B
RAIL_REQUEST, RAIL_GRANTED, RAIL_RELEASE = 0, 1, 2
RAIL_RATE = 4800  # 48 kHz / 10

# UI kinds and flags
K_GROUP, K_READOUT, K_TOGGLE, K_NUMBER, K_CHOICE, K_ACTION, K_TEXT = range(7)
F_RO, F_CONFIRM, F_ADV, F_LIVE, F_SETTING = 1, 2, 4, 8, 16
TOP = 0xFF  # as a parent: the top of the page. In VIEW: closed (0xFE = the top page is showing)
UI_OK, UI_RANGE, UI_READONLY, UI_BUSY, UI_UNKNOWN = range(5)
VIEW_S = 3.0

# INPUT keys and gestures
KEY_PLAY, KEY_NEXT, KEY_PREV, KEY_VOL_UP, KEY_VOL_DOWN, KEY_MUTE = 16, 17, 18, 19, 20, 21
G_CLICK, G_LONG, G_REPEAT = 1, 2, 3
ENC_VOLUME = 1

# AUDIO_CMD MEDIA actions
MEDIA_TOGGLE, MEDIA_PLAY, MEDIA_PAUSE, MEDIA_NEXT, MEDIA_PREV = 1, 2, 3, 4, 5
MEDIA_TUNE_UP, MEDIA_TUNE_DOWN, MEDIA_PRESET_NEXT, MEDIA_PRESET_PREV, MEDIA_PRESET_STORE = 6, 7, 8, 9, 10
MEDIA_SEEK_UP, MEDIA_SEEK_DOWN, MEDIA_PRESET_RECALL = 11, 12, 13
SRC_NONE, SRC_BLUETOOTH, SRC_RADIO = 0, 1, 2  # AUDIO_CMD source codes
AUDIO_NOW, AUDIO_SOURCE = 0x04, 0x87


def crc16(data: bytes) -> int:
    """CRC-16/CCITT-FALSE."""
    return binascii.crc_hqx(data, 0xFFFF)


def slot_node(slot: int) -> int:
    """A slot card's node (accessory_ports.md, Node numbers)."""
    return 0x14 + slot - 1


# ---- transfers (can_channels.md, Transport)

def transfer_frames(payload: bytes, tid: int) -> list[bytes]:
    """The frames of one transfer: tail byte SOT | EOT | toggle | transfer ID; CRC first when multi-frame."""
    if not 1 <= len(payload) <= 256:
        raise ValueError("transfer payload must be 1-256 bytes")
    tid &= 0x1F
    if len(payload) <= 7:
        return [payload + bytes([0xC0 | tid])]
    stream = struct.pack("<H", crc16(payload)) + payload
    frames = []
    for i in range(0, len(stream), 7):
        tail = (0x80 if i == 0 else 0) | (0x40 if i + 7 >= len(stream) else 0) | ((len(frames) & 1) << 5) | tid
        frames.append(stream[i:i + 7] + bytes([tail]))
    return frames


class Reassembler:
    """One buffer per CAN ID, as the receiving rules say (toggle, transfer ID, 100 ms gap, CRC, 1 s duplicates)."""

    def __init__(self) -> None:
        self.buf: dict[int, tuple[int, int, float, bytearray]] = {}  # id: (tid, next toggle, last time, data)
        self.done: dict[int, tuple[int, float]] = {}

    def feed(self, can_id: int, data: bytes, now: float) -> bytes | None:
        if not data:
            return None
        tail, body = data[-1], data[:-1]
        sot, eot, tog, tid = tail & 0x80, tail & 0x40, (tail >> 5) & 1, tail & 0x1F
        if sot:
            if tog:
                return None
            if eot:
                return self._complete(can_id, tid, bytes(body), now)
            self.buf[can_id] = (tid, 1, now, bytearray(body))
            return None
        cur = self.buf.get(can_id)
        if cur is None or cur[0] != tid or cur[1] != tog or now - cur[2] > 0.1:
            self.buf.pop(can_id, None)
            return None
        cur[3].extend(body)
        if len(cur[3]) > 258:
            self.buf.pop(can_id, None)
            return None
        if not eot:
            self.buf[can_id] = (tid, tog ^ 1, now, cur[3])
            return None
        self.buf.pop(can_id, None)
        stream = bytes(cur[3])
        if len(stream) < 3 or struct.unpack_from("<H", stream)[0] != crc16(stream[2:]):
            return None
        return self._complete(can_id, tid, stream[2:], now)

    def _complete(self, can_id: int, tid: int, payload: bytes, now: float) -> bytes | None:
        last = self.done.get(can_id)
        self.done[can_id] = (tid, now)
        if last and last[0] == tid and now - last[1] < 1.0:
            return None
        return payload


def lp(text: str, cap: int) -> bytes:
    """A string with its length byte in front (ONLINE)."""
    b = text.encode()[:cap]
    return bytes([len(b)]) + b


# ---- a card

class SimCard:
    """A node that speaks the channel protocol. Subclasses set the class attributes and override the hooks."""

    CMD = "card"                      # the relay command (:card ...) and the name in --no-sim / :sim
    NAME = "Card (sim)"               # ANNOUNCE name, <= 24 bytes
    LABEL = "card"                    # what the log calls it
    NODE_TYPE = 0
    PRODUCT = PRODUCT_NONE
    FIRMWARE = 0x0001
    UNIQUE_ID = 0                     # fixed, so the center's settings backup recognises the board between runs
    PRODUCES: tuple[str, ...] = ()    # channel names; NET is added to both maps, as ElcChannels::begin does
    CONSUMES: tuple[str, ...] = ()
    LISTENS: tuple[int, ...] = ()     # channel bases on_frame hands to receive() (NET is always handled)
    HELP = "status"

    def __init__(self, send: Callable[[int, bytes], None], log: Callable[[str], None], slot: int = 1,
                 seed: int | None = None, clock: Callable[[], float] | None = None, node: int | None = None) -> None:
        self.send_frame, self.log = send, log
        self.clock = clock or time.monotonic
        self.slot = slot
        self.node = node if node is not None else slot_node(slot)
        self.lock = threading.RLock()
        self.rx = Reassembler()
        self.tids: dict[int, int] = {}
        self.rand = random.Random(seed)
        self.start = self.clock()
        self.generation = 1
        self.factory = True
        self.nodes: dict[int, tuple[int, int, str]] = {}  # node: (type, product, name), from ANNOUNCE
        self.heard_at: dict[int, float] = {}              # node: its last heartbeat
        self.identify_until = 0.0
        self.pending: list[tuple[float, Callable[[], None]]] = []
        self.next_hb = 0.0
        self.view_group, self.view_at = TOP, 0.0

    # ---- sending

    def transfer(self, base: int, payload: bytes) -> None:
        can_id = base + self.node
        tid = self.tids.get(can_id, 0)
        self.tids[can_id] = (tid + 1) & 0x1F
        for f in transfer_frames(payload, tid):
            self.send_frame(can_id, f)

    def heartbeat(self) -> None:
        up = int(self.clock() - self.start)
        self.send_frame(HEARTBEAT + self.node, struct.pack("<BHIB", self.NODE_TYPE, self.FIRMWARE, up, 0x01))

    @classmethod
    def channel_map(cls, names: tuple[str, ...]) -> int:
        return sum(1 << CHANNEL_BIT[c] for c in set(names) | {"NET"})

    def announce(self) -> None:
        flags = (1 if self.factory else 0) | (self.slot & 7) << 1
        self.transfer(CH_NET, struct.pack("<BBBHHIBIIB", NET_ANNOUNCE, 1, self.NODE_TYPE, self.PRODUCT, self.FIRMWARE,
                                          self.UNIQUE_ID, self.generation, self.channel_map(self.PRODUCES),
                                          self.channel_map(self.CONSUMES), flags) + self.NAME.encode()[:24])

    def item(self, n: int, parent: int, kind: int, flags: int, label: str, unit: str = "", options: str = "",
             exp: int = 0, lo: int = 0, hi: int = 0, step: int = 0) -> None:
        text = "\0".join((label, unit, options)).rstrip("\0").encode()
        self.transfer(CH_UI, struct.pack("<BBBBBbiii", 0x01, n, parent, kind, flags, exp, lo, hi, step) + text)

    def ui_value(self, n: int, v: int) -> None:
        self.transfer(CH_UI, struct.pack("<BBi", 0x02, n, v))

    def ui_text(self, n: int, text: str) -> None:
        self.transfer(CH_UI, bytes([0x03, n]) + text.encode()[:64])

    def ui_result(self, n: int, result: int, asker: int) -> None:
        self.transfer(CH_UI, bytes([0x04, n, result, asker]))

    def sensor_define(self, n: int, quantity: int, exp: int, flags: int, period_ms: int, lo: int, hi: int,
                      name: str, caution: tuple[int, int] = (NONE32, NONE32),
                      warning: tuple[int, int] = (NONE32, NONE32)) -> None:
        self.transfer(CH_SENSOR, struct.pack("<BBBbBHiiiiii", 0x01, n, quantity, exp, flags, period_ms, lo, hi,
                                             caution[0], caution[1], warning[0], warning[1]) + name.encode()[:20])

    def sensor_value(self, n: int, v: int, status: int = 0) -> None:
        self.transfer(CH_SENSOR, struct.pack("<BBiB", 0x02, n, v, status))

    def rail_send(self, dest: int, state: int, rate: int = RAIL_RATE) -> None:
        self.transfer(CH_NET, struct.pack("<BBBBH", NET_RAIL, dest, self.slot, state, rate))

    def line(self, text: str, level: int = 1) -> None:
        self.transfer(CH_LOG, bytes([0x01, level]) + text.encode()[:120])
        self.log(text)

    # ---- the bus

    def find(self, ntype: int, product: int = 0) -> int:
        """First announced node of a type (and product, unless 0), or 0: ElcChannels::findNode."""
        for n in sorted(self.nodes):
            t, p, _ = self.nodes[n]
            if t == ntype and (not product or p == product):
                return n
        return 0

    def viewed(self, now: float) -> bool:
        """A page of this card is open in the menu (the last VIEW is under 3 s old and not 0xFF)."""
        return self.view_group != TOP and now - self.view_at < VIEW_S

    # ---- receiving

    def on_frame(self, can_id: int, data: bytes) -> None:
        now = self.clock()
        base, src = can_id & ~0x1F, can_id & 0x1F
        if src == self.node:
            return
        if HEARTBEAT <= can_id < HEARTBEAT + 0x20:
            with self.lock:
                self.heard_at[src] = now
            return
        if base != CH_NET and base not in self.LISTENS:
            return
        with self.lock:
            p = self.rx.feed(can_id, data, now)
            if not p:
                return
            op = p[0]
            if op >= 0x80 and (len(p) < 2 or p[1] not in (0, self.node)):
                return
            if base == CH_NET:
                self.net(src, op, p, now)
            elif base == CH_UI and op == 0x82 and len(p) >= 3:
                self.view_group, self.view_at = p[2], now
            else:
                self.receive(base, src, op, p, now)

    def net(self, src: int, op: int, p: bytes, now: float) -> None:
        if op == NET_ANNOUNCE and len(p) >= 6:
            ntype, product = p[2], struct.unpack_from("<H", p, 3)[0]
            name = p[21:].decode("utf-8", "replace") if len(p) > 21 else ""
            self.nodes[src] = (ntype, product, name)
            self.heard(src, ntype, product)
        elif op == NET_QUERY and len(p) >= 3:
            what = p[2]
            self.later(now + self.node * 0.008, lambda: self.answer_query(what))
        elif op == NET_IDENTIFY and len(p) >= 3:
            self.identify_until = now + p[2]
            self.log(f"IDENTIFY {p[2]} s (the LEDs would blink)")
        elif op == NET_RESTART and len(p) >= 3 and p[2] == 0x5A:
            self.log("RESTART")
            self.restart(now)
        elif op == NET_RAIL and len(p) >= 4:
            self.on_rail(src, p, now)
        else:
            self.receive(CH_NET, src, op, p, now)

    def answer_query(self, what: int) -> None:
        if what & 1:
            self.announce()
        if what & 2:
            self.sensor_defines()
        if what & 4:
            self.alert_defines()
        if what & 8:
            self.items()

    def restart(self, now: float) -> None:
        self.start = now
        self.later(now + 0.3, self.boot)

    # ---- hooks

    def heard(self, src: int, ntype: int, product: int) -> None:
        """A node ANNOUNCEd."""

    def on_rail(self, src: int, p: bytes, now: float) -> None:
        """NET RAIL addressed to this node (or to all)."""

    def receive(self, base: int, src: int, op: int, p: bytes, now: float) -> None:
        """A transfer on one of LISTENS (or a NET opcode the base does not handle)."""

    def periodic(self, now: float) -> None:
        """Called from tick() after the heartbeat."""

    def sensor_defines(self) -> None:
        pass

    def alert_defines(self) -> None:
        pass

    def items(self) -> None:
        pass

    def boot_text(self) -> str:
        return f"simulated {self.LABEL} at node 0x{self.node:02x} (slot {self.slot})"

    def command(self, words: list[str]) -> str | None:
        """A relay command (:card words...). Returns text to print, or None for the help line."""
        if not words or words[0] == "status":
            return self.status()
        return None

    def status(self) -> str:
        return f"node 0x{self.node:02x} slot {self.slot}"

    # ---- time

    def later(self, at: float, fn: Callable[[], None]) -> None:
        self.pending.append((at, fn))

    def boot(self) -> None:
        self.heartbeat()
        self.announce()
        self.sensor_defines()
        self.alert_defines()
        self.items()
        self.log(self.boot_text())

    def tick(self) -> None:
        now = self.clock()
        with self.lock:
            due = [p for p in self.pending if p[0] <= now]
            self.pending = [p for p in self.pending if p[0] > now]
            for _, fn in due:
                fn()
            if now >= self.next_hb:
                self.next_hb = now + 1.0
                self.heartbeat()
            self.periodic(now)


# ---- test helpers (used by the other modules' self-tests too)

class Clock:
    """A clock the tests move by hand."""

    def __init__(self, t: float = 1000.0) -> None:
        self.t = t

    def __call__(self) -> float:
        return self.t


class Bus:
    """Collects a card's frames and reassembles them into (can_id, payload) transfers."""

    def __init__(self, clock: Callable[[], float] = time.monotonic) -> None:
        self.frames: list[tuple[int, bytes]] = []
        self.lines: list[str] = []
        self.clock = clock
        self.rx = Reassembler()
        self.transfers: list[tuple[int, bytes]] = []

    def send(self, can_id: int, data: bytes) -> None:
        self.frames.append((can_id, data))
        if can_id < HEARTBEAT:
            p = self.rx.feed(can_id, data, self.clock())
            if p:
                self.transfers.append((can_id, p))

    def got(self, base: int, op: int | None = None) -> list[bytes]:
        return [p for i, p in self.transfers if i & ~0x1F == base and (op is None or p[0] == op)]

    def clear(self) -> None:
        self.frames.clear()
        self.transfers.clear()


def deliver(card: SimCard, src: int, base: int, payload: bytes, tid: int) -> None:
    """Hand one transfer from node `src` to a card."""
    for f in transfer_frames(payload, tid):
        card.on_frame(base + src, f)


def _test() -> None:
    assert crc16(b"123456789") == 0x29B1
    one = transfer_frames(b"\x01\x02", 5)
    assert one == [b"\x01\x02\xc5"]
    payload = bytes(range(40))
    frames = transfer_frames(payload, 31)
    assert all(len(f) == 8 for f in frames[:-1]) and frames[0][-1] & 0x80 and frames[-1][-1] & 0x40
    assert [(f[-1] >> 5) & 1 for f in frames] == [i & 1 for i in range(len(frames))]
    rx = Reassembler()
    got = [rx.feed(0x3D5, f, 0.0) for f in frames]
    assert got[-1] == payload and not any(got[:-1])
    assert rx.feed(0x3D5, frames[0], 0.5) is None  # same transfer ID within 1 s: a duplicate after completion
    bad = list(frames)
    bad[1] = bytes([bad[1][0] ^ 1]) + bad[1][1:]
    rx2 = Reassembler()
    assert [rx2.feed(0x3D5, f, 0.0) for f in bad][-1] is None  # CRC
    rx3 = Reassembler()
    assert rx3.feed(0x3D5, frames[0], 0.0) is None and rx3.feed(0x3D5, frames[1], 0.2) is None  # 100 ms gap
    # the multi-frame example in can_channels.md (ALERT DEFINE, transfer ID 3)
    ex = transfer_frames(bytes.fromhex("01000201010000") + b"Trans fluid hot", 3)
    assert ex == [bytes.fromhex(h) for h in ("5FC1010002010183", "00005472616E7323", "20666C7569642003",
                                              "686F7463")], [f.hex() for f in ex]
    assert lp("Austin", 32) == b"\x06Austin"

    clock = Clock()
    bus = Bus(clock)

    class Dummy(SimCard):
        NAME, NODE_TYPE, PRODUCT, UNIQUE_ID = "Dummy", 4, 0x8001, 0x1234
        PRODUCES, CONSUMES, LISTENS = ("LOG",), ("INPUT",), (CH_INPUT, CH_UI)

        def __init__(self, *a, **k) -> None:
            super().__init__(*a, **k)
            self.got: list[bytes] = []

        def receive(self, base: int, src: int, op: int, p: bytes, now: float) -> None:
            self.got.append(p)

    card = Dummy(bus.send, bus.lines.append, slot=3, clock=clock)
    assert card.node == 0x16
    card.boot()
    hb = [d for i, d in bus.frames if i == HEARTBEAT + 0x16]
    assert hb == [struct.pack("<BHIB", 4, 1, 0, 1)]
    ann = bus.got(CH_NET, NET_ANNOUNCE)[0]
    v = struct.unpack_from("<BBBHHIBIIB", ann)
    assert v[2:] == (4, 0x8001, 1, 0x1234, 1, (1 << 31) | (1 << 30), (1 << 2) | (1 << 30), 1 | 3 << 1), v
    assert ann[21:] == b"Dummy"
    bus.clear()
    deliver(card, 0x01, CH_NET, bytes([NET_QUERY, 0x00, 0x0F]), 0)
    assert not bus.transfers
    clock.t += 0.2
    card.tick()
    assert bus.got(CH_NET, NET_ANNOUNCE)                      # answered after node x 8 ms
    deliver(card, 0x01, CH_NET, bytes([NET_QUERY, 0x15, 0x0F]), 1)
    assert not card.pending                                    # addressed to another node
    deliver(card, 0x13, CH_INPUT, bytes([0x01, KEY_NEXT, G_CLICK, 1]), 0)
    assert card.got == [bytes([0x01, KEY_NEXT, G_CLICK, 1])]
    card.on_frame(HEARTBEAT + 0x14, struct.pack("<BHIB", 5, 1, 9, 1))
    assert card.heard_at[0x14] == clock.t
    deliver(card, 0x14, CH_NET, struct.pack("<BBBHHIBIIB", 1, 1, 5, 4, 1, 7, 1, 0, 0, 2) + b"Audio", 2)
    assert card.find(NODE_TYPE_SINK, PRODUCT_AUDIO_CARD) == 0x14 and card.nodes[0x14][2] == "Audio"
    deliver(card, 0x01, CH_UI, bytes([0x82, 0x16, 0xFE]), 0)
    assert card.viewed(clock.t)
    clock.t += 3.5
    assert not card.viewed(clock.t)
    deliver(card, 0x01, CH_UI, bytes([0x82, 0x16, 0xFF]), 1)
    assert not card.viewed(clock.t)
    deliver(card, 0x01, CH_NET, bytes([NET_RESTART, 0x16, 0x5A]), 3)
    bus.clear()
    clock.t += 0.5
    card.tick()
    assert bus.got(CH_NET, NET_ANNOUNCE) and any(i == HEARTBEAT + 0x16 for i, _ in bus.frames)
    print("chan_sim: all tests passed")


if __name__ == "__main__":
    import sys
    if "--test" in sys.argv:
        _test()
    else:
        print(__doc__)
