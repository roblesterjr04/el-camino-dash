#!/usr/bin/env python3
"""A simulated AM/FM radio card (docs/hardware/radio_card.md) on the channel protocol (docs/protocols/can_channels.md).

can_relay.py runs it so the dash can be worked on with no radio card built. It sends what the card's firmware will
send, framed as channel transfers (tail byte, multi-frame with CRC):

    heartbeat 0x700 + node   1 Hz, node type 9, flags bit0 (speaks the channel protocol)
    NET 0x3C0 + node         ANNOUNCE at start and on QUERY; RAIL request / release to the audio card
    UI 0x260 + node          the radio's page (Menu > Accessories > its name): items, VALUE / TEXT, RESULT
    SENSOR 0x120 + node      0 signal (dBuV), 1 SNR (dB), 1 s
    LOG 0x3E0 + node         tune and seek results

and acts on what it hears: NET QUERY / IDENTIFY / RESTART / RAIL, UI SET / VIEW, INPUT keys (next / previous seek,
play / pause mutes, only while the radio is the source), AUDIO_CMD STATUS (the sink's mute), SYSTEM POWER.

The stations are made up. Settings live in memory only, so every run starts at factory defaults (ANNOUNCE flag bit0).

    python3 tools/radio_sim.py --test     # transfer framing and the radio's own logic, no hardware
"""
from __future__ import annotations

import binascii
import random
import struct
import threading
import time
from typing import Callable

# ---- channels (can_channels.md, ID map): base + node

CH_INPUT, CH_SYSTEM, CH_SENSOR, CH_AUDIO_CMD, CH_UI, CH_NET, CH_LOG = 0x040, 0x060, 0x120, 0x240, 0x260, 0x3C0, 0x3E0
HEARTBEAT = 0x700
CHANNEL_BIT = {name: base >> 5 for name, base in (("INPUT", CH_INPUT), ("SYSTEM", CH_SYSTEM), ("SENSOR", CH_SENSOR),
                                                  ("AUDIO_CMD", CH_AUDIO_CMD), ("UI", CH_UI), ("NET", CH_NET),
                                                  ("LOG", CH_LOG))}

NODE_TYPE_RADIO = 9
NODE_TYPE_SINK = 5
PRODUCT_RADIO = 0x0009
PRODUCT_AUDIO_CARD = 0x0004
FIRMWARE = 0x0001
UNIQUE_ID = 0x5D0A0009  # fixed, so the center's settings backup recognises the simulated board between runs
NAME = "Radio (sim)"
NONE32 = -0x80000000

# UI kinds and flags
K_GROUP, K_READOUT, K_TOGGLE, K_NUMBER, K_CHOICE, K_ACTION, K_TEXT = range(7)
F_RO, F_CONFIRM, F_ADV, F_LIVE, F_SETTING = 1, 2, 4, 8, 16
TOP = 0xFF

# INPUT keys and gestures
KEY_PLAY, KEY_NEXT, KEY_PREV = 16, 17, 18
G_CLICK, G_REPEAT = 1, 3


def crc16(data: bytes) -> int:
    """CRC-16/CCITT-FALSE."""
    return binascii.crc_hqx(data, 0xFFFF)


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


# ---- the stations (made up)

class Station:
    def __init__(self, khz: int, ps: str, texts: list[str], stereo: bool, rssi: int, snr: int) -> None:
        self.khz, self.ps, self.texts, self.stereo, self.rssi, self.snr = khz, ps, texts, stereo, rssi, snr


STATIONS = [
    Station(88500, "KJZZ 885", ["Late Night Jazz - Blue Hour", "Members keep this station on the air"], True, 41, 22),
    Station(91300, "KPUB FM", ["Morning Edition", "Traffic and weather on the eights"], True, 47, 26),
    Station(94700, "THE WAVE", ["Now: Sea Breeze - Coastline", "Next: Low Tide - Harbor Lights"], True, 55, 31),
    Station(98300, "K-ROCK", ["Highway Song - The Drifters Band", "Rock that rolls, 98.3"], True, 52, 29),
    Station(101100, "CLASSIC", ["Summer Nights - The Cruisers (1964)", "The music of the sixties"], True, 49, 27),
    Station(104900, "KCTY", ["Two Lane Road - Mesa Creek", "Country all day"], True, 38, 17),
    Station(107500, "KXLR", [], False, 24, 7),
    Station(680, "", [], False, 44, 20),
    Station(1070, "", [], False, 36, 14),
    Station(1450, "", [], False, 27, 8),
]

BANDS = ("FM", "AM")
REGIONS = ("US", "Europe")
FM_RANGE = (87500, 108000)
AM_RANGE = (520, 1710)

# UI item numbers
I_ON, I_BAND, I_FREQ, I_SEEK_UP, I_SEEK_DN, I_STATION, I_TEXT, I_STEREO, I_SIGNAL = range(9)
I_RECALL, I_STORE, I_REGION, I_RSSI_MIN, I_SNR_MIN, I_MEMORY = 9, 10, 11, 12, 13, 14
I_PRESET0 = 15  # 15-26: the presets' frequencies (kHz, 0 = empty), the values the center backs up
PRESETS = 12
SEEK_S = 0.8
RDS_PS_S, RDS_RT_S, RDS_RT_ROTATE_S = 1.0, 3.0, 20.0


def fmt_khz(khz: int) -> str:
    return f"{khz / 1000:.1f} FM" if khz >= 10000 else f"{khz} AM"


class RadioSim:
    """The card's behaviour. `send(can_id, data)` puts one frame on the bus; `log(text)` prints on the relay."""

    def __init__(self, send: Callable[[int, bytes], None], log: Callable[[str], None], slot: int = 2,
                 seed: int | None = None) -> None:
        self.send_frame, self.log = send, log
        self.slot = slot
        self.node = 0x14 + slot - 1
        self.lock = threading.RLock()
        self.rx = Reassembler()
        self.tids: dict[int, int] = {}
        self.rand = random.Random(seed)
        self.start = time.monotonic()
        self.generation = 1
        self.factory = True
        # settings
        self.on, self.band, self.region = 1, 0, 0
        self.khz = {0: 101100, 1: 1070}
        self.presets = [0] * PRESETS
        self.rssi_min, self.snr_min = 20, 3
        # state
        self.source = False          # the selected source on the audio card (no source select exists yet)
        self.sink_node = 0           # the audio card, from its ANNOUNCE
        self.rail = 0                # 0 none, 1 requested, 2 granted
        self.sink_muted = False
        self.muted = False
        self.seek_dir, self.seek_at = 0, 0.0
        self.tuned_at = time.monotonic()
        self.view_group, self.view_at = TOP, 0.0
        self.next_hb = self.next_sensor = self.next_live = 0.0
        self.sent_ps, self.sent_rt = None, None
        self.identify_until = 0.0
        self.pending: list[tuple[float, Callable[[], None]]] = []

    # ---- sending

    def transfer(self, base: int, payload: bytes) -> None:
        can_id = base + self.node
        tid = self.tids.get(can_id, 0)
        self.tids[can_id] = (tid + 1) & 0x1F
        for f in transfer_frames(payload, tid):
            self.send_frame(can_id, f)

    def heartbeat(self) -> None:
        up = int(time.monotonic() - self.start)
        self.send_frame(HEARTBEAT + self.node, struct.pack("<BHIB", NODE_TYPE_RADIO, FIRMWARE, up, 0x01))

    def announce(self) -> None:
        produces = sum(1 << CHANNEL_BIT[c] for c in ("SENSOR", "UI", "NET", "LOG"))
        consumes = sum(1 << CHANNEL_BIT[c] for c in ("INPUT", "SYSTEM", "AUDIO_CMD", "NET"))
        flags = (1 if self.factory else 0) | (self.slot & 7) << 1
        self.transfer(CH_NET, struct.pack("<BBBHHIBIIB", 0x01, 1, NODE_TYPE_RADIO, PRODUCT_RADIO, FIRMWARE, UNIQUE_ID,
                                          self.generation, produces, consumes, flags) + NAME.encode()[:24])

    def sensor_defines(self) -> None:
        for n, name in ((0, "Radio signal dBuV"), (1, "Radio SNR dB")):
            self.transfer(CH_SENSOR, struct.pack("<BBBbBHiiiiii", 0x01, n, 0, 0, 0x01, 1000, 0, 127,
                                                 NONE32, NONE32, NONE32, NONE32) + name.encode()[:20])

    def item(self, n: int, parent: int, kind: int, flags: int, label: str, unit: str = "", options: str = "",
             exp: int = 0, lo: int = 0, hi: int = 0, step: int = 0) -> None:
        text = "\0".join((label, unit, options)).rstrip("\0").encode()
        self.transfer(CH_UI, struct.pack("<BBBBBbiii", 0x01, n, parent, kind, flags, exp, lo, hi, step) + text)

    def items(self) -> None:
        fm = self.band == 0
        eu = self.region == 1
        self.item(I_ON, TOP, K_TOGGLE, F_SETTING, "Radio on")
        self.item(I_BAND, TOP, K_CHOICE, F_SETTING, "Band", options="\n".join(BANDS))
        if fm:
            self.item(I_FREQ, TOP, K_NUMBER, F_SETTING, "Frequency", "MHz", exp=-1, lo=875, hi=1080, step=1)
        else:
            self.item(I_FREQ, TOP, K_NUMBER, F_SETTING, "Frequency", "kHz", lo=AM_RANGE[0] if not eu else 522,
                      hi=AM_RANGE[1], step=9 if eu else 10)
        self.item(I_SEEK_UP, TOP, K_ACTION, 0, "Seek up")
        self.item(I_SEEK_DN, TOP, K_ACTION, 0, "Seek down")
        self.item(I_STATION, TOP, K_TEXT, F_RO, "Station")
        self.item(I_TEXT, TOP, K_TEXT, F_RO, "Radio text")
        self.item(I_STEREO, TOP, K_READOUT, F_RO | F_LIVE, "Stereo")
        self.item(I_SIGNAL, TOP, K_READOUT, F_RO | F_LIVE, "Signal", "dBuV")
        self.item(I_RECALL, TOP, K_CHOICE, 0, "Preset", options=self.preset_options())
        self.item(I_STORE, TOP, K_CHOICE, F_CONFIRM, "Store as preset", options=self.preset_options())
        self.item(I_REGION, TOP, K_CHOICE, F_SETTING, "Region", options="\n".join(REGIONS))
        self.item(I_RSSI_MIN, TOP, K_NUMBER, F_ADV | F_SETTING, "Seek signal min", "dBuV", lo=0, hi=127, step=1)
        self.item(I_SNR_MIN, TOP, K_NUMBER, F_ADV | F_SETTING, "Seek SNR min", "dB", lo=0, hi=127, step=1)
        self.item(I_MEMORY, TOP, K_GROUP, F_ADV, "Preset memory")
        for i in range(PRESETS):
            self.item(I_PRESET0 + i, I_MEMORY, K_NUMBER, F_ADV | F_SETTING, f"Preset {i + 1}", "kHz",
                      lo=0, hi=FM_RANGE[1], step=1)
        for n in range(I_PRESET0 + PRESETS):
            self.send_value(n)

    def preset_options(self) -> str:
        return "\n".join(f"{i + 1}  {fmt_khz(k) if k else '--'}" for i, k in enumerate(self.presets))

    def value_of(self, n: int) -> int | None:
        st = self.station()
        if n == I_ON:
            return self.on
        if n == I_BAND:
            return self.band
        if n == I_FREQ:
            k = self.khz[self.band]
            return k // 100 if self.band == 0 else k
        if n == I_STEREO:
            return 1 if st and st.stereo and self.band == 0 and self.on else 0
        if n == I_SIGNAL:
            return self.rssi()
        if n in (I_RECALL, I_STORE):
            return 0
        if n == I_REGION:
            return self.region
        if n == I_RSSI_MIN:
            return self.rssi_min
        if n == I_SNR_MIN:
            return self.snr_min
        if I_PRESET0 <= n < I_PRESET0 + PRESETS:
            return self.presets[n - I_PRESET0]
        return None

    def send_value(self, n: int) -> None:
        if n in (I_STATION, I_TEXT):
            self.send_text(n)
            return
        v = self.value_of(n)
        if v is not None:
            self.transfer(CH_UI, struct.pack("<BBi", 0x02, n, v))

    def send_text(self, n: int) -> None:
        ps, rt = self.rds()
        t = ps if n == I_STATION else rt
        self.transfer(CH_UI, bytes([0x03, n]) + t.encode()[:64])
        if n == I_STATION:
            self.sent_ps = ps
        else:
            self.sent_rt = rt

    def line(self, text: str, level: int = 1) -> None:
        self.transfer(CH_LOG, bytes([0x01, level]) + text.encode()[:120])
        self.log(text)

    # ---- the radio

    def station(self) -> Station | None:
        if self.seek_dir:
            return None
        k = self.khz[self.band]
        return next((s for s in STATIONS if s.khz == k), None)

    def rssi(self) -> int:
        st = self.station()
        if not self.on:
            return 0
        base = st.rssi if st else 6
        return max(0, base + self.rand.randint(-3, 3))

    def snr(self) -> int:
        st = self.station()
        if not self.on:
            return 0
        base = st.snr if st else 1
        return max(0, base + self.rand.randint(-2, 2))

    def rds(self) -> tuple[str, str]:
        st = self.station()
        if not st or not self.on or self.band != 0:
            return "", ""
        age = time.monotonic() - self.tuned_at
        ps = st.ps if age >= RDS_PS_S else ""
        rt = ""
        if st.texts and age >= RDS_RT_S:
            rt = st.texts[int((age - RDS_RT_S) // RDS_RT_ROTATE_S) % len(st.texts)]
        return ps, rt

    def grid(self) -> int:
        if self.band == 0:
            return 100
        return 9 if self.region == 1 else 10

    def band_range(self) -> tuple[int, int]:
        if self.band == 0:
            return FM_RANGE
        return (522, 1710) if self.region == 1 else AM_RANGE

    def tune(self, khz: int, why: str = "tuned") -> None:
        lo, hi = self.band_range()
        khz = max(lo, min(hi, khz))
        self.khz[self.band] = khz
        self.seek_dir = 0
        self.tuned_at = time.monotonic()
        self.sent_ps = self.sent_rt = None
        for n in (I_FREQ, I_STEREO, I_SIGNAL, I_STATION, I_TEXT):
            self.send_value(n)
        st = self.station()
        self.line(f"{why} {fmt_khz(khz)}" + (f", {st.ps.strip() or 'no RDS'}" if st else ", no station"))

    def seek(self, direction: int) -> None:
        if not self.on:
            return
        self.seek_dir = direction
        self.seek_at = time.monotonic() + SEEK_S
        self.sent_ps = self.sent_rt = None
        self.send_value(I_STATION)
        self.send_value(I_TEXT)

    def seek_result(self) -> int:
        """The next station past the thresholds in the seek direction, wrapping round the band."""
        k = self.khz[self.band]
        lo, hi = self.band_range()
        cands = sorted(s.khz for s in STATIONS if lo <= s.khz <= hi and (s.khz >= 10000) == (self.band == 0)
                       and s.rssi >= self.rssi_min and s.snr >= self.snr_min)
        if not cands:
            return k
        if self.seek_dir > 0:
            return next((c for c in cands if c > k), cands[0])
        return next((c for c in reversed(cands) if c < k), cands[-1])

    def set_on(self, on: int) -> None:
        self.on = on
        if not on:
            self.seek_dir = 0
            self.release_rail()
        else:
            self.tuned_at = time.monotonic()
        self.line(f"radio {'on' if on else 'off'}")
        for n in (I_ON, I_STEREO, I_SIGNAL, I_STATION, I_TEXT):
            self.send_value(n)
        self.update_rail()

    # ---- the rail (NET RAIL)

    def update_rail(self) -> None:
        want = self.on and self.source and self.sink_node
        if want and self.rail == 0:
            self.rail = 1
            self.transfer(CH_NET, struct.pack("<BBBBH", 0x84, self.sink_node, self.slot, 0, 4800))
            self.log(f"asks the audio card 0x{self.sink_node:02x} for RET")
        elif not want:
            self.release_rail()

    def release_rail(self) -> None:
        if self.rail and self.sink_node:
            self.transfer(CH_NET, struct.pack("<BBBBH", 0x84, self.sink_node, self.slot, 2, 4800))
            self.log("releases RET")
        self.rail = 0

    def set_source(self, on: bool) -> None:
        with self.lock:
            self.source = on
            self.log(f"radio {'is' if on else 'is not'} the selected source"
                     + ("" if self.sink_node or not on else " (no audio card heard: keys act, no RAIL request)"))
            self.update_rail()

    # ---- receiving

    def on_frame(self, can_id: int, data: bytes) -> None:
        now = time.monotonic()
        base, src = can_id & ~0x1F, can_id & 0x1F
        if src == self.node or base not in (CH_NET, CH_UI, CH_INPUT, CH_AUDIO_CMD, CH_SYSTEM):
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
            elif base == CH_UI:
                self.ui(src, op, p, now)
            elif base == CH_INPUT and op == 0x01 and len(p) >= 3:
                self.key(p[1], p[2])
            elif base == CH_AUDIO_CMD and op == 0x01 and len(p) >= 3:
                muted = bool(p[2] & 1)
                if muted != self.sink_muted:
                    self.sink_muted = muted
                    self.log(f"sink {'muted' if muted else 'unmuted'}: radio {'pauses' if muted else 'resumes'}")
            elif base == CH_SYSTEM and op == 0x04 and len(p) >= 2:
                if p[1] == 1:
                    self.line(f"ignition off: saved {fmt_khz(self.khz[self.band])}, muted")
                    self.muted = True
                elif p[1] == 2:
                    self.muted = False

    def net(self, src: int, op: int, p: bytes, now: float) -> None:
        if op == 0x01 and len(p) >= 6:
            ntype, product = p[2], struct.unpack_from("<H", p, 3)[0]
            if ntype == NODE_TYPE_SINK and product == PRODUCT_AUDIO_CARD and self.sink_node != src:
                self.sink_node = src
                self.log(f"audio card heard at 0x{src:02x}")
                self.update_rail()
        elif op == 0x81 and len(p) >= 3:
            what = p[2]
            self.later(now + self.node * 0.008, lambda: self.answer_query(what))
        elif op == 0x82 and len(p) >= 3:
            self.identify_until = now + p[2]
            self.log(f"IDENTIFY {p[2]} s (the LEDs would blink)")
        elif op == 0x83 and len(p) >= 3 and p[2] == 0x5A:
            self.log("RESTART")
            self.rail = 0
            self.start = now
            self.later(now + 0.3, self.boot)
        elif op == 0x84 and len(p) >= 4 and src == self.sink_node:
            state = p[3]
            if state == 1 and self.rail:
                self.rail = 2
                self.log("RET granted: the RET buffer would be enabled")
            elif state == 2:
                was = self.rail
                self.rail = 0
                self.log("RET withdrawn or denied: buffer off within 10 ms" if was else "RET denied")

    def answer_query(self, what: int) -> None:
        if what & 1:
            self.announce()
        if what & 2:
            self.sensor_defines()
        if what & 8:
            self.items()

    def ui(self, src: int, op: int, p: bytes, now: float) -> None:
        if op == 0x82 and len(p) >= 3:
            self.view_group, self.view_at = p[2], now
            return
        if op != 0x81 or len(p) < 7:
            return
        n, v = p[2], struct.unpack_from("<i", p, 3)[0]
        result = self.set(n, v)
        self.send_value(n)
        self.transfer(CH_UI, bytes([0x04, n, result, src]))

    def set(self, n: int, v: int) -> int:
        """Apply a SET from the menu. Result 0 ok, 1 out of range, 2 read-only, 4 unknown item."""
        if n in (I_STATION, I_TEXT, I_STEREO, I_SIGNAL, I_MEMORY):
            return 2
        if n == I_ON:
            if v not in (0, 1):
                return 1
            if v != self.on:
                self.set_on(v)
        elif n == I_BAND:
            if v not in (0, 1):
                return 1
            if v != self.band:
                self.band = v
                self.bump()
                self.tune(self.khz[v], "band")
        elif n == I_FREQ:
            khz = v * 100 if self.band == 0 else v
            lo, hi = self.band_range()
            if not lo <= khz <= hi or (khz - lo) % self.grid():
                return 1
            self.tune(khz)
        elif n in (I_SEEK_UP, I_SEEK_DN):
            if v != 1:
                return 1
            self.seek(1 if n == I_SEEK_UP else -1)
            return 0
        elif n == I_RECALL:
            if not 0 <= v < PRESETS:
                return 1
            k = self.presets[v]
            if not k:
                return 3
            band = 0 if k >= 10000 else 1
            if band != self.band:
                self.band = band
                self.bump()
                self.send_value(I_BAND)
            self.tune(k, f"preset {v + 1}")
        elif n == I_STORE:
            if not 0 <= v < PRESETS:
                return 1
            self.presets[v] = self.khz[self.band]
            self.line(f"stored {fmt_khz(self.presets[v])} as preset {v + 1}")
            self.bump()
            self.send_value(I_PRESET0 + v)
        elif n == I_REGION:
            if v not in (0, 1):
                return 1
            if v != self.region:
                self.region = v
                self.bump()
                if self.band == 1:
                    lo = self.band_range()[0]
                    self.tune(lo + round((self.khz[1] - lo) / self.grid()) * self.grid(), "region")
        elif n in (I_RSSI_MIN, I_SNR_MIN):
            if not 0 <= v <= 127:
                return 1
            if n == I_RSSI_MIN:
                self.rssi_min = v
            else:
                self.snr_min = v
        elif I_PRESET0 <= n < I_PRESET0 + PRESETS:
            if v and not (FM_RANGE[0] <= v <= FM_RANGE[1] or 522 <= v <= AM_RANGE[1]):
                return 1
            self.presets[n - I_PRESET0] = v
            self.bump()
        else:
            return 4
        self.factory = False
        return 0

    def bump(self) -> None:
        """A description changed (the frequency's range, the preset list): new generation, items sent again."""
        self.generation = (self.generation % 255) + 1
        self.items()

    def key(self, key: int, gesture: int) -> None:
        if not self.source or not self.on or gesture not in (G_CLICK, G_REPEAT):
            return
        if key in (KEY_NEXT, KEY_PREV):
            self.seek(1 if key == KEY_NEXT else -1)
        elif key == KEY_PLAY and gesture == G_CLICK:
            self.muted = not self.muted
            self.line(f"radio {'muted' if self.muted else 'unmuted'}")

    # ---- time

    def later(self, at: float, fn: Callable[[], None]) -> None:
        self.pending.append((at, fn))

    def boot(self) -> None:
        self.heartbeat()
        self.announce()
        self.sensor_defines()
        self.items()
        self.log(f"simulated radio card at node 0x{self.node:02x} (slot {self.slot}), {fmt_khz(self.khz[self.band])}")

    def tick(self) -> None:
        now = time.monotonic()
        with self.lock:
            due = [p for p in self.pending if p[0] <= now]
            self.pending = [p for p in self.pending if p[0] > now]
            for _, fn in due:
                fn()
            if now >= self.next_hb:
                self.next_hb = now + 1.0
                self.heartbeat()
            if self.seek_dir and now >= self.seek_at:
                self.tune(self.seek_result(), "seek")
            if now >= self.next_sensor:
                self.next_sensor = now + 1.0
                for n, v in ((0, self.rssi()), (1, self.snr())):
                    self.transfer(CH_SENSOR, struct.pack("<BBiB", 0x02, n, v, 0 if self.on else 1))
            ps, rt = self.rds()
            if ps != self.sent_ps:
                self.send_text(I_STATION)
            if rt != self.sent_rt:
                self.send_text(I_TEXT)
            if self.view_group != TOP and now - self.view_at < 3.0 and now >= self.next_live:
                self.next_live = now + 1.0
                self.send_value(I_SIGNAL)
                self.send_value(I_STEREO)

    def status(self) -> str:
        ps, rt = self.rds()
        rail = ("no audio card", "requested", "granted")[self.rail] if self.sink_node else "no audio card"
        return (f"node 0x{self.node:02x} slot {self.slot}, {'on' if self.on else 'off'}, "
                f"{fmt_khz(self.khz[self.band])}{' seeking' if self.seek_dir else ''}, '{ps}' '{rt}', "
                f"signal {self.rssi()} dBuV, source {'yes' if self.source else 'no'}, RET {rail}, "
                f"presets {' '.join(fmt_khz(k) if k else '--' for k in self.presets)}")


# ---- tests (no hardware)

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

    sent: list[tuple[int, bytes]] = []
    lines: list[str] = []
    sim = RadioSim(lambda i, d: sent.append((i, d)), lines.append, slot=2, seed=1)
    sim.boot()
    rx3 = Reassembler()
    items = {}
    for i, d in sent:
        p = rx3.feed(i, d, 0.0)
        if p and i == CH_UI + sim.node and p[0] == 0x01:
            items[p[1]] = p[18:].split(b"\0")[0].decode()
    assert items[I_FREQ] == "Frequency" and items[I_PRESET0 + 11] == "Preset 12", items
    assert any(i == CH_NET + 0x15 for i, _ in sent) and any(i == HEARTBEAT + 0x15 for i, _ in sent)

    tid = [0]

    def ui_set(item: int, value: int, frm: int = 0x01) -> None:
        tid[0] += 1
        sim.on_frame(CH_UI + frm, transfer_frames(struct.pack("<BBBi", 0x81, sim.node, item, value), tid[0])[0])

    ui_set(I_FREQ, 989)                       # 98.9 MHz: in range, on the grid, no station
    assert sim.khz[0] == 98900 and sim.station() is None
    ui_set(I_FREQ, 1200)                      # out of range: refused
    assert sim.khz[0] == 98900
    sim.seek_dir = 1
    assert sim.seek_result() == 101100
    sim.seek_dir = -1
    assert sim.seek_result() == 98300
    sim.seek_dir = 0
    sim.rssi_min = 45
    sim.seek_dir = 1
    assert sim.seek_result() == 101100      # 104.9 and 107.5 are under 45 dBuV
    sim.khz[0] = 101100
    assert sim.seek_result() == 91300       # wraps
    sim.seek_dir = 0
    sim.rssi_min = 20
    sim.tune(94700)
    ui_set(I_STORE, 2)
    assert sim.presets[2] == 94700 and not sim.factory
    ui_set(I_BAND, 1)
    assert sim.band == 1 and sim.khz[1] == 1070
    ui_set(I_RECALL, 2)
    assert sim.band == 0 and sim.khz[0] == 94700
    sim.source = False
    sim.on_frame(CH_INPUT + 0x13, transfer_frames(bytes([0x01, KEY_NEXT, G_CLICK, 1]), 0)[0])
    assert sim.seek_dir == 0                 # not the source: keys ignored
    sim.set_source(True)
    sim.on_frame(CH_INPUT + 0x13, transfer_frames(bytes([0x01, KEY_NEXT, G_CLICK, 2]), 1)[0])
    assert sim.seek_dir == 1
    sim.seek_dir = 0
    sent.clear()
    ann = struct.pack("<BBBHHIBIIB", 0x01, 1, NODE_TYPE_SINK, PRODUCT_AUDIO_CARD, 1, 7, 1, 0, 0, 2) + b"Audio"
    for f in transfer_frames(ann, 4):
        sim.on_frame(CH_NET + 0x14, f)
    assert sim.sink_node == 0x14 and sim.rail == 1
    rail = [d for i, d in sent if i == CH_NET + sim.node]
    assert rail and rail[0][:4] == bytes([0x84, 0x14, 2, 0]), rail
    sim.on_frame(CH_NET + 0x14, transfer_frames(struct.pack("<BBBBH", 0x84, sim.node, 1, 1, 4800), 5)[0])
    assert sim.rail == 2
    sim.on_frame(CH_NET + 0x14, transfer_frames(struct.pack("<BBBBH", 0x84, sim.node, 1, 2, 4800), 6)[0])
    assert sim.rail == 0
    print("radio_sim: all tests passed")


if __name__ == "__main__":
    import sys
    if "--test" in sys.argv:
        _test()
    else:
        print(__doc__)
