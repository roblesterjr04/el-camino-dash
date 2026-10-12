#!/usr/bin/env python3
"""A simulated AM/FM radio card (docs/hardware/radio_card.md) on the channel protocol (docs/protocols/can_channels.md).

can_relay.py runs it so the dash can be worked on with no radio card built. It sends what the card's firmware will
send, framed as channel transfers (tail byte, multi-frame with CRC):

    heartbeat 0x700 + node   1 Hz, node type 9, flags bit0 (speaks the channel protocol)
    NET 0x3C0 + node         ANNOUNCE at start and on QUERY; RAIL request / release to the audio card
    UI 0x260 + node          the radio's page (Menu > Accessories > its name): items, VALUE / TEXT, RESULT
    SENSOR 0x120 + node      0 signal (dBuV), 1 SNR (dB), 1 s
    LOG 0x3E0 + node         tune and seek results

and acts on what it hears: NET QUERY / IDENTIFY / RESTART / RAIL, UI SET / VIEW, AUDIO_CMD STATUS (the sink's mute)
and MEDIA addressed to it (next / previous seek, play / pause mutes: the audio card forwards these while the radio is
its source), SYSTEM POWER. With no audio card heard it acts on the INPUT keys itself while it is the source. Withdrawn
or denied RET, it asks again every 2 s while it still wants it. The shared framing and NET handling are in chan_sim.py.

The stations are made up. Settings live in memory only, so every run starts at factory defaults (ANNOUNCE flag bit0).

    python3 tools/radio_sim.py --test     # transfer framing and the radio's own logic, no hardware
"""
from __future__ import annotations

import struct
from typing import Callable

from chan_sim import (CH_AUDIO_CMD, CH_INPUT, CH_NET, CH_SENSOR, CH_SYSTEM, CH_UI, CHANNEL_BIT, F_ADV,  # noqa: F401
                      F_CONFIRM, F_LIVE, F_RO, F_SETTING, G_CLICK, G_REPEAT, HEARTBEAT, K_ACTION, K_CHOICE, K_GROUP,
                      K_NUMBER, K_READOUT, K_TEXT, K_TOGGLE, KEY_NEXT, KEY_PLAY, KEY_PREV, MEDIA_NEXT, MEDIA_PAUSE,
                      MEDIA_PLAY, MEDIA_PREV, MEDIA_TOGGLE, NODE_TYPE_RADIO, NODE_TYPE_SINK, NONE32,
                      MEDIA_PRESET_NEXT, MEDIA_PRESET_PREV, MEDIA_PRESET_STORE, MEDIA_SEEK_DOWN, MEDIA_SEEK_UP,
                      MEDIA_TUNE_DOWN, MEDIA_TUNE_UP, SRC_RADIO, AUDIO_NOW, MEDIA_PRESET_RECALL,
                      PRODUCT_AUDIO_CARD, PRODUCT_RADIO, TOP, Reassembler, SimCard, crc16, transfer_frames)

FIRMWARE = 0x0001
UNIQUE_ID = 0x5D0A0009  # fixed, so the center's settings backup recognises the simulated board between runs
NAME = "Radio (sim)"
RAIL_RETRY_S = 2.0


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
FM_RANGE = (87500, 108000)  # every FM frequency either region allows (Europe's band; presets are checked against it)
FM_US = (87900, 107900)      # US: the odd tenths, 200 kHz apart
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


class RadioSim(SimCard):
    """The card's behaviour. `send(can_id, data)` puts one frame on the bus; `log(text)` prints on the relay."""

    CMD = "radio"
    NAME = NAME
    NODE_TYPE = NODE_TYPE_RADIO
    PRODUCT = PRODUCT_RADIO
    FIRMWARE = FIRMWARE
    UNIQUE_ID = UNIQUE_ID
    PRODUCES = ("SENSOR", "UI", "NET", "LOG")
    CONSUMES = ("INPUT", "SYSTEM", "NET")  # it reads the sink's AUDIO_CMD STATUS and MEDIA to it, but is no sink
    LISTENS = (CH_UI, CH_INPUT, CH_AUDIO_CMD, CH_SYSTEM)
    HELP = "status | on | off | source on|off | tune 98.3 | seek up|down"

    def __init__(self, send: Callable[[int, bytes], None], log: Callable[[str], None], slot: int = 2,
                 seed: int | None = None, clock: Callable[[], float] | None = None) -> None:
        super().__init__(send, log, slot=slot, seed=seed, clock=clock)
        # settings
        self.on, self.band, self.region = 1, 0, 0
        self.khz = {0: 101100, 1: 1070}
        self.presets = [0] * PRESETS
        self.store_order: list[int] = list(range(PRESETS))  # oldest stored first
        self.sent_now, self.next_now = b'', 0.0
        self.rssi_min, self.snr_min = 20, 3
        # state
        self.source = False          # the selected source on the audio card (the relay tells it: no CAN message)
        self.sink_node = 0           # the audio card, from its ANNOUNCE
        self.rail = 0                # 0 none, 1 requested, 2 granted
        self.rail_retry = 0.0        # withdrawn or denied: ask again then, if it still wants RET
        self.sink_muted = False
        self.muted = False
        self.seek_dir, self.seek_at = 0, 0.0
        self.tuned_at = self.clock()
        self.next_sensor = self.next_live = 0.0
        self.sent_ps, self.sent_rt = None, None

    # ---- sending

    def sensor_defines(self) -> None:
        for n, name in ((0, "Radio signal dBuV"), (1, "Radio SNR dB")):
            self.sensor_define(n, 0, 0, 0x01, 1000, 0, 127, name)

    def items(self) -> None:
        fm = self.band == 0
        eu = self.region == 1
        self.item(I_ON, TOP, K_TOGGLE, F_SETTING, "Radio on")
        self.item(I_BAND, TOP, K_CHOICE, F_SETTING, "Band", options="\n".join(BANDS))
        if fm:
            lo, hi = self.band_range()
            self.item(I_FREQ, TOP, K_NUMBER, F_SETTING, "Frequency", "MHz", exp=-1, lo=lo // 100, hi=hi // 100,
                      step=self.grid() // 100)
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
            self.ui_value(n, v)

    def send_text(self, n: int) -> None:
        ps, rt = self.rds()
        t = ps if n == I_STATION else rt
        self.ui_text(n, t)
        if n == I_STATION:
            self.sent_ps = ps
        else:
            self.sent_rt = rt

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
        age = self.clock() - self.tuned_at
        ps = st.ps if age >= RDS_PS_S else ""
        rt = ""
        if st.texts and age >= RDS_RT_S:
            rt = st.texts[int((age - RDS_RT_S) // RDS_RT_ROTATE_S) % len(st.texts)]
        return ps, rt

    def grid(self, band: int | None = None) -> int:
        if (self.band if band is None else band) == 0:
            return 100 if self.region == 1 else 200
        return 9 if self.region == 1 else 10

    def band_range(self, band: int | None = None) -> tuple[int, int]:
        if (self.band if band is None else band) == 0:
            return FM_RANGE if self.region == 1 else FM_US
        return (522, 1710) if self.region == 1 else AM_RANGE

    def snap(self, band: int, khz: int) -> int:
        """The nearest frequency on the band's grid for the region."""
        lo, hi = self.band_range(band)
        g = self.grid(band)
        return max(lo, min(hi, lo + round((khz - lo) / g) * g))

    def tune(self, khz: int, why: str = "tuned") -> None:
        khz = self.snap(self.band, khz)  # on the region's grid, whatever asked (an old preset at 101.2 plays 101.1)
        self.khz[self.band] = khz
        self.seek_dir = 0
        self.tuned_at = self.clock()
        self.sent_ps = self.sent_rt = None
        for n in (I_FREQ, I_STEREO, I_SIGNAL, I_STATION, I_TEXT):
            self.send_value(n)
        st = self.station()
        self.line(f"{why} {fmt_khz(khz)}" + (f", {st.ps.strip() or 'no RDS'}" if st else ", no station"))

    def seek(self, direction: int) -> None:
        if not self.on:
            return
        self.seek_dir = direction
        self.seek_at = self.clock() + SEEK_S
        self.sent_ps = self.sent_rt = None
        self.send_value(I_STATION)
        self.send_value(I_TEXT)

    def preset_index(self) -> int | None:
        k = self.khz[self.band]
        return next((i for i, p in enumerate(self.presets) if p == k), None)

    def recall(self, i: int) -> None:
        k = self.presets[i]
        band = 0 if k >= 10000 else 1
        if band != self.band:
            self.band = band
            self.bump()
            self.send_value(I_BAND)
        self.tune(k, f"preset {i + 1}")

    def now_playing(self) -> bytes:
        """AUDIO_CMD NOW PLAYING: the frequency and station name, and the radio text."""
        ps, rt = self.rds()
        st = self.station()
        line1 = fmt_khz(self.khz[self.band]) + (f"  {ps.strip()}" if ps.strip() else "")
        if self.seek_dir:
            line1 = "Seeking " + ("up" if self.seek_dir > 0 else "down")
        flags = (1 if st and st.stereo and self.band == 0 else 0) | (2 if ps else 0) | (4 if self.muted else 0) \
            | (8 if self.seek_dir else 0)
        pi = self.preset_index()
        bars = max(0, min(5, (self.rssi() - 10) // 9)) if self.on else 0
        l1, l2 = line1.encode()[:32], rt.encode()[:64]
        return bytes([AUDIO_NOW, SRC_RADIO, flags, (pi + 1) if pi is not None else 0, bars, len(l1)]) + l1 + \
            bytes([len(l2)]) + l2

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
            self.tuned_at = self.clock()
        self.line(f"radio {'on' if on else 'off'}")
        for n in (I_ON, I_STEREO, I_SIGNAL, I_STATION, I_TEXT):
            self.send_value(n)
        self.update_rail()

    # ---- the rail (NET RAIL)

    def update_rail(self) -> None:
        want = self.on and self.source and self.sink_node
        if want and self.rail == 0:
            self.rail = 1
            self.rail_retry = 0.0
            self.rail_send(self.sink_node, 0)
            self.log(f"asks the audio card 0x{self.sink_node:02x} for RET")
        elif not want:
            self.release_rail()

    def release_rail(self) -> None:
        if self.rail and self.sink_node:
            self.rail_send(self.sink_node, 2)
            self.log("releases RET")
        self.rail = 0
        self.rail_retry = 0.0

    def set_source(self, on: bool) -> None:
        with self.lock:
            self.source = on
            self.log(f"radio {'is' if on else 'is not'} the selected source"
                     + ("" if self.sink_node or not on else " (no audio card heard: keys act, no RAIL request)"))
            self.update_rail()

    # ---- receiving

    def receive(self, base: int, src: int, op: int, p: bytes, now: float) -> None:
        if base == CH_UI:
            self.ui(src, op, p, now)
        elif base == CH_INPUT and op == 0x01 and len(p) >= 3:
            self.key(p[1], p[2])
        elif base == CH_AUDIO_CMD and op == 0x01 and len(p) >= 3:
            muted = bool(p[2] & 1)
            if muted != self.sink_muted:
                self.sink_muted = muted
                self.log(f"sink {'muted' if muted else 'unmuted'}: radio {'pauses' if muted else 'resumes'}")
            if len(p) >= 8 and src == self.sink_node:  # the sink names the source: is it this radio?
                chosen = p[6] == SRC_RADIO and p[7] == self.node
                if chosen != self.source:
                    if chosen and not self.on:
                        self.set_on(1)  # choosing the radio turns it on
                    self.set_source(chosen)
        elif base == CH_AUDIO_CMD and op == 0x84 and len(p) >= 3 and p[1] == self.node:
            self.media(p[2], p[3] if len(p) > 3 else 0)
        elif base == CH_SYSTEM and op == 0x04 and len(p) >= 2:
            if p[1] == 1:
                self.line(f"ignition off: saved {fmt_khz(self.khz[self.band])}, muted")
                self.muted = True
            elif p[1] == 2:
                self.muted = False

    def heard(self, src: int, ntype: int, product: int) -> None:
        if ntype == NODE_TYPE_SINK and self.sink_node != src:  # the audio card (or a DevKit bridge as the sink)
            self.sink_node = src
            self.log(f"audio card heard at 0x{src:02x}")
            self.update_rail()

    def restart(self, now: float) -> None:
        self.rail = 0
        super().restart(now)

    def on_rail(self, src: int, p: bytes, now: float) -> None:
        if src != self.sink_node:
            return
        state = p[3]
        if state == 1 and self.rail:
            self.rail = 2
            self.log("RET granted: the RET buffer would be enabled")
        elif state == 2:
            was = self.rail
            self.rail = 0
            self.rail_retry = now + RAIL_RETRY_S
            self.log("RET withdrawn: buffer off within 10 ms" if was == 2 else "RET denied")

    def ui(self, src: int, op: int, p: bytes, now: float) -> None:
        if op != 0x81 or len(p) < 7:
            return
        n, v = p[2], struct.unpack_from("<i", p, 3)[0]
        result = self.set(n, v)
        self.send_value(n)
        self.ui_result(n, result, src)

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
            if v in self.store_order:
                self.store_order.remove(v)
            self.store_order.append(v)
            self.line(f"stored {fmt_khz(self.presets[v])} as preset {v + 1}")
            self.bump()
            self.send_value(I_PRESET0 + v)
        elif n == I_REGION:
            if v not in (0, 1):
                return 1
            if v != self.region:
                self.region = v
                self.bump()
                other = 1 - self.band  # both bands onto the new grid (US FM: the odd tenths)
                self.khz[other] = self.snap(other, self.khz[other])
                self.tune(self.snap(self.band, self.khz[self.band]), "region")
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
        """INPUT keys. The audio sink acts on the media keys, so once an audio card is heard the radio leaves them to it
        and takes MEDIA from the sink instead; with no audio card (the radio alone on the bench) it acts on them."""
        if self.sink_node or not self.source or not self.on or gesture not in (G_CLICK, G_REPEAT):
            return
        if key in (KEY_NEXT, KEY_PREV):
            self.seek(1 if key == KEY_NEXT else -1)
        elif key == KEY_PLAY and gesture == G_CLICK:
            self.muted = not self.muted
            self.line(f"radio {'muted' if self.muted else 'unmuted'}")

    def media(self, action: int, value: int = 0) -> None:
        """AUDIO_CMD MEDIA addressed to the radio: the audio card forwards them while the radio is the source."""
        if not self.on:
            return
        if action in (MEDIA_NEXT, MEDIA_PREV, MEDIA_SEEK_UP, MEDIA_SEEK_DOWN):
            self.seek(1 if action in (MEDIA_NEXT, MEDIA_SEEK_UP) else -1)
        elif action in (MEDIA_TUNE_UP, MEDIA_TUNE_DOWN):
            g = self.grid()
            lo, hi = self.band_range()
            k = self.khz[self.band] + (g if action == MEDIA_TUNE_UP else -g)
            self.tune(hi if k < lo else (lo if k > hi else k), "tuned")  # wraps round the band
        elif action in (MEDIA_PRESET_NEXT, MEDIA_PRESET_PREV):
            stored = [i for i, k in enumerate(self.presets) if k]
            if not stored:
                self.line("no presets stored", 2)
                return
            cur = self.preset_index()
            step = 1 if action == MEDIA_PRESET_NEXT else -1
            if cur is None:
                i = stored[0] if step > 0 else stored[-1]
            else:
                pos = stored.index(cur) if cur in stored else 0
                i = stored[(pos + step) % len(stored)]
            self.recall(i)
        elif action == MEDIA_PRESET_RECALL:
            if 1 <= value <= PRESETS and self.presets[value - 1]:
                self.recall(value - 1)
            else:
                self.line(f"preset {value} is empty", 2)
        elif action == MEDIA_PRESET_STORE:
            if 1 <= value <= PRESETS:
                slot = value - 1
            else:
                free = [i for i, k in enumerate(self.presets) if not k]
                slot = free[0] if free else self.store_order[0]
            self.set(I_STORE, slot)
            self.send_value(I_PRESET0 + slot)
        elif action in (MEDIA_TOGGLE, MEDIA_PLAY, MEDIA_PAUSE):
            muted = not self.muted if action == MEDIA_TOGGLE else action == MEDIA_PAUSE
            if muted != self.muted:
                self.muted = muted
                self.line(f"radio {'muted' if self.muted else 'unmuted'}")

    # ---- time

    def boot_text(self) -> str:
        return f"simulated radio card at node 0x{self.node:02x} (slot {self.slot}), {fmt_khz(self.khz[self.band])}"

    def periodic(self, now: float) -> None:
        if self.rail == 0 and self.rail_retry and now >= self.rail_retry:
            self.update_rail()
        if self.rail == 2 and now - self.heard_at.get(self.sink_node, now) > 3.0:
            self.rail = 0
            self.rail_retry = now + RAIL_RETRY_S
            self.log("audio card's heartbeat lost: RET buffer off")
        if self.seek_dir and now >= self.seek_at:
            self.tune(self.seek_result(), "seek")
        if now >= self.next_sensor:
            self.next_sensor = now + 1.0
            for n, v in ((0, self.rssi()), (1, self.snr())):
                self.sensor_value(n, v, 0 if self.on else 1)
        ps, rt = self.rds()
        if ps != self.sent_ps:
            self.send_text(I_STATION)
        if rt != self.sent_rt:
            self.send_text(I_TEXT)
        if self.source and self.on:  # NOW PLAYING while this radio is the source: on change and every 2 s
            np = self.now_playing()
            if np[:2] + np[6:] != self.sent_now[:2] + self.sent_now[6:] or now >= self.next_now:
                self.sent_now, self.next_now = np, now + 2.0
                self.transfer(CH_AUDIO_CMD, np)
        if self.viewed(now) and now >= self.next_live:
            self.next_live = now + 1.0
            self.send_value(I_SIGNAL)
            self.send_value(I_STEREO)

    def command(self, words: list[str]) -> str | None:
        cmd = words[0] if words else "status"
        arg = words[1] if len(words) > 1 else ""
        if cmd == "status":
            return self.status()
        if cmd in ("on", "off"):
            self.set(I_ON, 1 if cmd == "on" else 0)
            self.send_value(I_ON)
        elif cmd == "source":
            self.set_source(arg != "off")
        elif cmd == "tune" and arg:
            try:
                f = float(arg)
            except ValueError:
                return "tune 98.3 (MHz) or 1070 (kHz)"
            band = 0 if f < 200 else 1
            if band != self.band:
                self.set(I_BAND, band)
                self.send_value(I_BAND)
            if self.set(I_FREQ, round(f * 10) if band == 0 else int(f)):
                return "not a frequency on this band's grid"
        elif cmd == "seek":
            self.seek(-1 if arg == "down" else 1)
        else:
            return None
        return ""

    def status(self) -> str:
        ps, rt = self.rds()
        rail = ("none", "requested", "granted")[self.rail] if self.sink_node else "no audio card"
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
    ui_set(I_FREQ, 990)                       # 99.0 MHz: an even tenth, off the US grid: refused
    assert sim.khz[0] == 98900
    ui_set(I_REGION, 1)                       # Europe: 100 kHz steps, and 87.5-108.0
    ui_set(I_FREQ, 990)
    assert sim.khz[0] == 99000
    ui_set(I_REGION, 0)                       # back to the US: onto the odd tenths
    assert sim.khz[0] in (98900, 99100)
    sim.tune(98900)
    sim.tune(101200)                          # an even tenth from an old preset: onto the grid
    assert sim.khz[0] in (101100, 101300)
    sim.tune(98900)
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
    # withdrawn: asks again after RAIL_RETRY_S while it still wants RET
    sim.rail_retry = sim.clock() - 0.1
    sent.clear()
    sim.tick()
    assert sim.rail == 1 and any(d[:4] == bytes([0x84, 0x14, 2, 0]) for i, d in sent if i == CH_NET + sim.node)
    # with an audio card heard, the media keys are the sink's: it forwards them as MEDIA
    sim.on_frame(CH_INPUT + 0x13, transfer_frames(bytes([0x01, KEY_NEXT, G_CLICK, 3]), 2)[0])
    assert sim.seek_dir == 0
    sim.on_frame(CH_AUDIO_CMD + 0x14, transfer_frames(bytes([0x84, sim.node, MEDIA_PREV]), 7)[0])
    assert sim.seek_dir == -1
    sim.seek_dir = 0
    sim.on_frame(CH_AUDIO_CMD + 0x14, transfer_frames(bytes([0x84, 0x16, MEDIA_NEXT]), 8)[0])
    assert sim.seek_dir == 0                 # MEDIA for another node
    sim.muted = False
    sim.on_frame(CH_AUDIO_CMD + 0x14, transfer_frames(bytes([0x84, sim.node, MEDIA_TOGGLE]), 9)[0])
    assert sim.muted
    # the sink's STATUS names the radio as the source: it turns on; MEDIA tunes, stores and recalls presets
    t = [100.0]
    out: list[tuple[int, bytes]] = []
    r = RadioSim(lambda i, d: out.append((i, d)), lambda x: None, slot=2, seed=2, clock=lambda: t[0])
    r.boot()
    r.set(I_ON, 0)
    r.on_frame(HEARTBEAT + 0x14, b"\x05\x01\x00\x00\x00\x00\x00\x01")
    r.heard(0x14, NODE_TYPE_SINK, PRODUCT_AUDIO_CARD)
    rx4 = Reassembler()
    tid = [0]

    def from_sink(payload: bytes) -> None:
        tid[0] += 1
        for f in transfer_frames(payload, tid[0]):
            r.on_frame(CH_AUDIO_CMD + 0x14, f)

    from_sink(bytes([0x01, 30, 0, 0, 0, 0x0F, SRC_RADIO, r.node]))
    assert r.on and r.source and r.rail == 1
    r.tune(94700)
    from_sink(bytes([0x84, r.node, MEDIA_TUNE_UP, 0]))
    assert r.khz[0] == 94900                 # US FM: one step is 0.2 MHz, odd tenths only
    from_sink(bytes([0x84, r.node, MEDIA_PRESET_STORE, 0]))
    assert r.presets[0] == 94900
    r.tune(101100)
    from_sink(bytes([0x84, r.node, MEDIA_PRESET_STORE, 0]))
    assert r.presets[1] == 101100
    from_sink(bytes([0x84, r.node, MEDIA_PRESET_NEXT, 0]))
    assert r.khz[0] == 94900
    out.clear()
    t[0] += 3
    r.periodic(t[0])
    nows = [p for i, d in out for p in [rx4.feed(i, d, 0.0)] if p and i == CH_AUDIO_CMD + r.node and p[0] == AUDIO_NOW]
    assert nows and nows[-1][1] == SRC_RADIO and nows[-1][3] == 1 and b"94.9 FM" in nows[-1]
    from_sink(bytes([0x01, 30, 0, 0, 0, 0x0F, 1, 0x14]))  # back to Bluetooth: it lets RET go
    assert not r.source and r.rail == 0
    print("radio_sim: all tests passed")


if __name__ == "__main__":
    import sys
    if "--test" in sys.argv:
        _test()
    else:
        print(__doc__)
