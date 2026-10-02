#!/usr/bin/env python3
"""Simulated slot cards on the channel protocol (docs/protocols/can_channels.md), for can_relay.py.

Every card the spec describes but the bench does not have, as its firmware will behave on CAN. The radio is in
radio_sim.py; the sensor card is real hardware on its own path (the center publishes the core sensors), so it is not
simulated here. Each card: heartbeat 0x700 + node at 1 Hz, NET ANNOUNCE at boot and on QUERY, IDENTIFY, RESTART
(chan_sim.SimCard), plus:

    audio   AudioCard      slot 1, node 0x14, node type 5 (audio sink), product 0x0004
            AUDIO_CMD STATUS at 1 Hz and on change; PLAY / STOP / VOLUME / MEDIA, DONE when a sound ends, ducking for
            priority >= 2; NET RAIL arbitration (one RET holder; the voice card outranks the others, the radio ranks
            lowest; RET freed when the holder's heartbeat stops for 3 s); the media keys and encoder 1 on INPUT; a UI
            page (Source Bluetooth / Radio, Volume, Fader, Balance). With Source = Radio it forwards play / next /
            previous to the radio (found from its ANNOUNCE, node type 9) as MEDIA addressed to it.
    gps     GpsCard        slot 3, node 0x16, product 0x0005
            SENSOR speed (km/h), heading, altitude, satellites, following the relay's simulated drive; a UI page (fix,
            satellites, speed). No fix for the first 6 s.
    lte     LteGateway     slot 4, node 0x17, node type 7, product 0x0006
            NET GATEWAY STATUS at 1 Hz; ONLINE PLACE (on a new place and every 30 s), WEATHER (at data up, then every
            15 min), TIMEZONE (every 5 min), ONLINE STATUS at 1 Hz, a MESSAGE 60 s after start (repeated every 30 s
            until MESSAGE READ); answers REFRESH, MESSAGE READ, GATEWAY CMD 4 (data cap); a UI page (network, signal,
            data used, data cap, look up now).
    voice   VoiceCard      slot 5, node 0x18, node type 8, product 0x0007
            asks the audio card for RET on `say`, speaks for 3 s once granted, then releases it.
    trans   TransTempCard  slot 6, node 0x19 (can_channels.md, Examples: "A transmission-temperature card")
            SENSOR 0 trans temp at 5 Hz; its own ALERTs (0 caution "Trans fluid hot", 1 warning) with STATE at 1 Hz,
            or with `mode center` the example's center-raised alerts (SENSOR flag bit2) instead; UI item 0 Offset.

The values are made up. Settings live in memory only. Every card takes `send`, `log`, `slot`, `clock` like RadioSim.

    python3 tools/cards_sim.py --test     # every card's logic, no hardware
"""
from __future__ import annotations

import math
import struct
import time
from typing import Callable

from chan_sim import (CH_ALERT, CH_AUDIO_CMD, CH_INPUT, CH_NET, CH_ONLINE, CH_SENSOR, CH_SYSTEM, CH_UI, F_LIVE,
                      F_RO, F_SETTING, G_CLICK, G_REPEAT, HEARTBEAT, K_ACTION, K_CHOICE, K_NUMBER, K_READOUT, K_TEXT,
                      KEY_MUTE, KEY_NEXT, KEY_PLAY, KEY_PREV, KEY_VOL_DOWN, KEY_VOL_UP, MEDIA_NEXT, MEDIA_PAUSE,
                      MEDIA_PLAY, MEDIA_PREV, MEDIA_TOGGLE, NET_GATEWAY_CMD, NET_GATEWAY_STATUS, NODE_TYPE_GATEWAY,
                      NODE_TYPE_RADIO, NODE_TYPE_SENSOR, NODE_TYPE_SINK, NODE_TYPE_VOICE, PRODUCT_AUDIO_CARD,
                      PRODUCT_GPS, PRODUCT_LTE, PRODUCT_NONE, PRODUCT_VOICE, RAIL_GRANTED, RAIL_RELEASE,
                      RAIL_REQUEST, TOP, UI_OK, UI_RANGE, UI_READONLY, UI_UNKNOWN, Bus, Clock, SimCard, deliver, lp)

try:
    from zoneinfo import ZoneInfo
except ImportError:  # Python < 3.9
    ZoneInfo = None  # type: ignore

Send = Callable[[int, bytes], None]
Log = Callable[[str], None]
Now = Callable[[], float]


def wrap16(d: int) -> int:
    """The difference of two cumulative s16 positions, across the wrap."""
    return ((d + 0x8000) & 0xFFFF) - 0x8000


# ==== the audio card (AudioCarrier/README.md, accessory_ports.md I2S rail rules)

# Built-in sounds (Registries, Sounds): name, length of one play in s (the ticks include their gap).
SOUNDS = {1: ("UI click", 0.05), 2: ("soft chime", 1.2), 3: ("caution chime", 1.5), 4: ("warning chime", 2.0),
          5: ("welcome", 3.0), 6: ("turn-signal tick", 0.4), 7: ("parking tick, slow", 1.0),
          8: ("parking tick, medium", 0.5), 9: ("parking tick, fast", 0.25), 10: ("continuous tone", 1.0)}
TRACKS = ["Highway Song - The Drifters Band", "Summer Nights - The Cruisers", "Two Lane Road - Mesa Creek",
          "Sea Breeze - Coastline"]
SOURCES = ("Bluetooth", "Radio")
A_SOURCE, A_VOLUME, A_FADER, A_BALANCE = range(4)
SERVICE_NODE = 0x1F  # sounds started from the relay's :audio play count as a service tool's
HOLDER_LOST_S = 3.0


def rail_rank(ntype: int) -> int:
    """Who may take RET from whom: a voice card over anything, the radio (a continuous source) under everything."""
    return {NODE_TYPE_VOICE: 2, NODE_TYPE_RADIO: 0}.get(ntype, 1)


class Sound:
    def __init__(self, prio: int, zones: int, level: int, ends: float | None) -> None:
        self.prio, self.zones, self.level, self.ends = prio, zones, level, ends


class AudioCard(SimCard):
    CMD = "audio"
    NAME = "Audio (sim)"
    NODE_TYPE = NODE_TYPE_SINK
    PRODUCT = PRODUCT_AUDIO_CARD
    UNIQUE_ID = 0x5D0A0004
    PRODUCES = ("AUDIO_CMD", "UI", "LOG")
    CONSUMES = ("AUDIO_CMD", "INPUT")
    LISTENS = (CH_AUDIO_CMD, CH_INPUT, CH_UI)
    HELP = ("status | source bt|radio | volume 0-100 | mute | play <sound 1-10> [priority 0-4] [repeat] | "
            "stop [sound] | media play|next|prev")

    def __init__(self, send: Send, log: Log, slot: int = 1, seed: int | None = None, clock: Now | None = None) -> None:
        super().__init__(send, log, slot=slot, seed=seed, clock=clock)
        self.volume, self.muted = 30, False
        self.source, self.fader, self.balance = 0, 0, 0
        self.bt_playing, self.track = False, 0
        self.sounds: dict[tuple[int, int], Sound] = {}
        self.holder, self.holder_since = 0, 0.0
        self.enc: dict[int, int] = {}        # node: last encoder 1 position
        self.key_seq: dict[int, int] = {}    # node: last KEY sequence
        self.sent_status: tuple[int, ...] | None = None
        self.next_status = 0.0
        self.on_source: Callable[[bool], None] | None = None  # the relay: tell the radio it is (not) the source

    # ---- state

    def radio_node(self) -> int:
        return self.find(NODE_TYPE_RADIO)

    def ducked(self) -> bool:
        voice = self.holder and rail_rank(self.nodes.get(self.holder, (0, 0, ""))[0]) > 0
        return any(s.prio >= 2 for s in self.sounds.values()) or bool(voice)

    def playing(self) -> bool:
        if self.source == 0:
            return self.bt_playing
        return bool(self.holder) and self.holder == self.radio_node()

    def status_tuple(self) -> tuple[int, ...]:
        flags = (1 if self.muted else 0) | (2 if self.playing() else 0) | (4 if self.ducked() else 0)
        return self.volume, flags, min(len(self.sounds), 255), 0, 0x0F

    def send_status(self) -> None:
        st = self.status_tuple()
        self.sent_status = st
        self.transfer(CH_AUDIO_CMD, struct.pack("<BBBBBB", 0x01, *st))

    def describe_status(self) -> str:
        v, f, n, _, _ = self.status_tuple()
        return (f"volume {v}{' muted' if f & 1 else ''}{', playing' if f & 2 else ''}{', ducked' if f & 4 else ''}, "
                f"{n} sound{'s' if n != 1 else ''}")

    # ---- UI page

    def items(self) -> None:
        self.item(A_SOURCE, TOP, K_CHOICE, F_SETTING, "Source", options="\n".join(SOURCES))
        self.item(A_VOLUME, TOP, K_NUMBER, 0, "Volume", "%", lo=0, hi=100, step=1)
        self.item(A_FADER, TOP, K_NUMBER, F_SETTING, "Fader", "rear-front", lo=-10, hi=10, step=1)
        self.item(A_BALANCE, TOP, K_NUMBER, F_SETTING, "Balance", "left-right", lo=-10, hi=10, step=1)
        for n in range(4):
            self.send_value(n)

    def send_value(self, n: int) -> None:
        self.ui_value(n, (self.source, self.volume, self.fader, self.balance)[n])

    def set(self, n: int, v: int) -> int:
        if n == A_SOURCE:
            if v not in (0, 1):
                return UI_RANGE
            self.set_source(v)
        elif n == A_VOLUME:
            if not 0 <= v <= 100:
                return UI_RANGE
            self.set_volume(v)
            return UI_OK
        elif n in (A_FADER, A_BALANCE):
            if not -10 <= v <= 10:
                return UI_RANGE
            if n == A_FADER:
                self.fader = v
            else:
                self.balance = v
            self.log(f"fader {self.fader:+d}, balance {self.balance:+d}")
        else:
            return UI_UNKNOWN
        self.factory = False
        return UI_OK

    def set_source(self, v: int) -> None:
        if v == self.source:
            return
        self.source = v
        self.line(f"source {SOURCES[v]}")
        self.send_value(A_SOURCE)
        if self.on_source:
            self.on_source(v == 1)

    def set_volume(self, v: int) -> None:
        v = max(0, min(100, v))
        if v != self.volume or self.muted:
            self.volume, self.muted = v, False
            self.send_value(A_VOLUME)

    def toggle_mute(self) -> None:
        self.muted = not self.muted
        self.line("muted" if self.muted else "unmuted")

    # ---- media

    def media(self, action: int) -> None:
        if self.source == 1:
            radio = self.radio_node()
            if not radio:
                self.log("source Radio, but no radio card heard: media key dropped")
                return
            self.transfer(CH_AUDIO_CMD, bytes([0x84, radio, action]))
            self.log(f"media {action} forwarded to the radio 0x{radio:02x}")
            return
        if action in (MEDIA_TOGGLE, MEDIA_PLAY, MEDIA_PAUSE):
            self.bt_playing = not self.bt_playing if action == MEDIA_TOGGLE else action == MEDIA_PLAY
            self.line(f"Bluetooth {'plays' if self.bt_playing else 'paused'}: {TRACKS[self.track]}")
        elif action in (MEDIA_NEXT, MEDIA_PREV):
            self.track = (self.track + (1 if action == MEDIA_NEXT else -1)) % len(TRACKS)
            self.line(f"Bluetooth {'next' if action == MEDIA_NEXT else 'previous'} track: {TRACKS[self.track]}")

    def key(self, src: int, key: int, gesture: int, seq: int) -> None:
        if self.key_seq.get(src) == seq:
            return
        self.key_seq[src] = seq
        if key in (KEY_VOL_UP, KEY_VOL_DOWN) and gesture in (G_CLICK, G_REPEAT):
            self.set_volume(self.volume + (2 if key == KEY_VOL_UP else -2))
        elif gesture != G_CLICK:
            return
        elif key == KEY_PLAY:
            self.media(MEDIA_TOGGLE)
        elif key in (KEY_NEXT, KEY_PREV):
            self.media(MEDIA_NEXT if key == KEY_NEXT else MEDIA_PREV)
        elif key == KEY_MUTE:
            self.toggle_mute()

    # ---- sounds

    def play(self, src: int, sound: int, zp: int, level: int, repeat: int, now: float) -> None:
        if sound not in SOUNDS:
            self.line(f"PLAY from 0x{src:02x}: no sound 0x{sound:04x} on this sink", 2)
            return
        zones, prio = (zp >> 4) or 0x0F, zp & 0x0F
        for k, s in list(self.sounds.items()):
            if k != (src, sound) and s.prio < prio and s.zones & zones:
                del self.sounds[k]
                self.done(k)
                self.log(f"{SOUNDS[k[1]][0]} from 0x{k[0]:02x} interrupted")
        length = SOUNDS[sound][1]
        self.sounds[(src, sound)] = Sound(prio, zones, level, now + length * repeat if repeat else None)
        self.log(f"plays {SOUNDS[sound][0]} for 0x{src:02x} (priority {prio}, zones 0x{zones:x}, level {level} %, "
                 f"{'until STOP' if not repeat else f'{repeat}x'}){', music ducked' if prio >= 2 else ''}")

    def stop(self, src: int, sound: int) -> None:
        keys = [k for k in self.sounds if k[0] == src and (sound == 0xFFFF or k[1] == sound)]
        for k in keys:
            del self.sounds[k]
            self.log(f"stopped {SOUNDS[k[1]][0]} for 0x{src:02x}")

    def done(self, k: tuple[int, int]) -> None:
        self.transfer(CH_AUDIO_CMD, struct.pack("<BBH", 0x02, k[0], k[1]))

    # ---- receiving

    def receive(self, base: int, src: int, op: int, p: bytes, now: float) -> None:
        if base == CH_UI:
            if op == 0x81 and len(p) >= 7:
                n, v = p[2], struct.unpack_from("<i", p, 3)[0]
                result = self.set(n, v) if n < 4 else UI_UNKNOWN
                if n < 4:
                    self.send_value(n)
                self.ui_result(n, result, src)
        elif base == CH_INPUT:
            if op == 0x01 and len(p) >= 4:
                self.key(src, p[1], p[2], p[3])
            elif op == 0x02 and len(p) >= 4 and p[1] == 1:
                pos = struct.unpack_from("<h", p, 2)[0]
                last = self.enc.get(src)
                self.enc[src] = pos
                if last is not None and pos != last:
                    self.set_volume(self.volume + wrap16(pos - last))
        elif base == CH_AUDIO_CMD:
            if op == 0x81 and len(p) >= 7:
                self.play(src, struct.unpack_from("<H", p, 2)[0], p[4], p[5], p[6], now)
            elif op == 0x82 and len(p) >= 4:
                self.stop(src, struct.unpack_from("<H", p, 2)[0])
            elif op == 0x83 and len(p) >= 4:
                action, value = p[2], struct.unpack_from("<b", p, 3)[0]
                if action == 0:
                    self.set_volume(value)
                elif action == 1:
                    self.set_volume(self.volume + value)
                elif action == 2:
                    self.toggle_mute()
            elif op == 0x84 and len(p) >= 3:
                self.media(p[2])
            elif op == 0x85 and len(p) >= 2:
                self.transfer(CH_AUDIO_CMD, bytes([0x03, 0, p[2] if len(p) > 2 else 0, p[3] if len(p) > 3 else 0]))
                self.log(f"STREAM_OPEN from 0x{src:02x} refused (streams are not simulated)")

    def on_rail(self, src: int, p: bytes, now: float) -> None:
        if p[1] != self.node:
            return
        state = p[3]
        name = self.nodes.get(src, (0, 0, f"0x{src:02x}"))[2] or f"0x{src:02x}"
        if state == RAIL_REQUEST:
            if self.holder in (0, src):
                self.grant(src, name, now)
            elif rail_rank(self.nodes.get(src, (0,))[0]) > rail_rank(self.nodes.get(self.holder, (0,))[0]):
                self.rail_send(self.holder, RAIL_RELEASE)
                self.line(f"RET withdrawn from 0x{self.holder:02x} for {name}")
                self.grant(src, name, now)
            else:
                self.rail_send(src, RAIL_RELEASE)
                self.log(f"RET denied to {name}: 0x{self.holder:02x} holds it")
        elif state == RAIL_RELEASE and src == self.holder:
            self.holder = 0
            self.log(f"RET released by {name}")

    def grant(self, src: int, name: str, now: float) -> None:
        self.holder, self.holder_since = src, now
        self.rail_send(src, RAIL_GRANTED)
        self.log(f"RET granted to {name} (0x{src:02x}): its audio mixed in"
                 + (", music ducked" if self.ducked() else ""))

    # ---- time

    def periodic(self, now: float) -> None:
        for k, s in list(self.sounds.items()):
            if s.ends is not None and now >= s.ends:
                del self.sounds[k]
                self.done(k)
        if self.holder and now - max(self.heard_at.get(self.holder, 0.0), self.holder_since) > HOLDER_LOST_S:
            self.line(f"RET freed: 0x{self.holder:02x}'s heartbeat stopped", 2)
            self.holder = 0
        if now >= self.next_status or self.status_tuple() != self.sent_status:
            self.next_status = now + 1.0
            self.send_status()

    def boot_text(self) -> str:
        return f"simulated audio card at node 0x{self.node:02x} (slot {self.slot}), source {SOURCES[self.source]}"

    def command(self, words: list[str]) -> str | None:
        cmd = words[0] if words else "status"
        args = words[1:]
        try:
            if cmd == "status":
                return self.status()
            if cmd == "source" and args and args[0] in ("bt", "bluetooth", "radio"):
                self.set_source(1 if args[0] == "radio" else 0)
            elif cmd == "volume" and args:
                self.set_volume(int(args[0]))
            elif cmd == "mute":
                self.toggle_mute()
            elif cmd == "play" and args:
                prio = int(args[1]) if len(args) > 1 else 1
                self.play(SERVICE_NODE, int(args[0], 0), prio & 0x0F, 100, int(args[2]) if len(args) > 2 else 1,
                          self.clock())
            elif cmd == "stop":
                self.stop(SERVICE_NODE, int(args[0], 0) if args else 0xFFFF)
            elif cmd == "media" and args and args[0] in ("play", "next", "prev"):
                self.media({"play": MEDIA_TOGGLE, "next": MEDIA_NEXT, "prev": MEDIA_PREV}[args[0]])
            else:
                return None
        except ValueError:
            return None
        return ""

    def status(self) -> str:
        holder = f"0x{self.holder:02x}" if self.holder else "free"
        sounds = ", ".join(f"{SOUNDS[k[1]][0]} (0x{k[0]:02x})" for k in self.sounds) or "none"
        return (f"node 0x{self.node:02x} slot {self.slot}, source {SOURCES[self.source]}, {self.describe_status()}, "
                f"fader {self.fader:+d} balance {self.balance:+d}, RET {holder}, sounds: {sounds}, "
                f"Bluetooth {'playing' if self.bt_playing else 'paused'} '{TRACKS[self.track]}'")


# ==== the GPS card (docs/hardware/gps_card.md)

G_FIX, G_SATS, G_SPEED = range(3)
FIX_S = 6.0


class GpsCard(SimCard):
    CMD = "gps"
    NAME = "GPS (sim)"
    LABEL = "GPS card"
    NODE_TYPE = NODE_TYPE_SENSOR
    PRODUCT = PRODUCT_GPS
    UNIQUE_ID = 0x5D0A0005
    PRODUCES = ("SENSOR", "UI", "LOG")
    LISTENS = (CH_UI,)
    HELP = "status | fix on|off"

    def __init__(self, send: Send, log: Log, slot: int = 3, seed: int | None = None, clock: Now | None = None,
                 speed_kmh: Callable[[], float] | None = None) -> None:
        super().__init__(send, log, slot=slot, seed=seed, clock=clock)
        self.speed_kmh = speed_kmh or (lambda: 0.0)
        self.fix_at: float | None = self.start + FIX_S
        self.fix = False
        self.sats, self.sats_at = 0, 0.0
        self.heading, self.distance, self.speed = 72.0, 0.0, 0.0
        self.last = self.start
        self.next_fast = self.next_slow = self.next_live = 0.0
        self.sent_fix: str | None = None

    def altitude(self) -> int:
        return int(round(165 + 12 * math.sin(self.distance / 800.0)))

    def fix_text(self) -> str:
        if self.fix:
            return f"3D fix, {self.sats} satellites"
        return f"Searching ({self.sats} satellites)"

    def sensor_defines(self) -> None:
        self.sensor_define(0, 6, -1, 0x03, 200, 0, 2000, "GPS speed")
        self.sensor_define(1, 13, 0, 0x01, 1000, 0, 359, "GPS heading")
        self.sensor_define(2, 10, 0, 0x01, 1000, -100, 3000, "GPS altitude")
        self.sensor_define(3, 15, 0, 0x01, 1000, 0, 24, "GPS satellites")

    def items(self) -> None:
        self.item(G_FIX, TOP, K_TEXT, F_RO, "Fix")
        self.item(G_SATS, TOP, K_READOUT, F_RO | F_LIVE, "Satellites")
        self.item(G_SPEED, TOP, K_READOUT, F_RO | F_LIVE, "Speed", "km/h", exp=-1)
        self.send_fix()
        self.ui_value(G_SATS, self.sats)
        self.ui_value(G_SPEED, int(round(self.speed * 10)))

    def send_fix(self) -> None:
        self.sent_fix = self.fix_text()
        self.ui_text(G_FIX, self.sent_fix)

    def receive(self, base: int, src: int, op: int, p: bytes, now: float) -> None:
        if base == CH_UI and op == 0x81 and len(p) >= 7:
            n = p[2]
            self.ui_result(n, UI_READONLY if n <= G_SPEED else UI_UNKNOWN, src)

    def periodic(self, now: float) -> None:
        dt = min(max(now - self.last, 0.0), 1.0)
        self.last = now
        self.speed = max(0.0, self.speed_kmh()) if self.fix else 0.0
        self.distance += self.speed / 3.6 * dt
        if self.speed > 5:
            self.heading = (self.heading + 4.0 * math.sin(self.distance / 400.0) * dt) % 360
        if not self.fix and self.fix_at is not None and now >= self.fix_at:
            self.fix = True
            self.line(f"3D fix after {now - self.start:.0f} s")
            self.sats_at = now
        if now >= self.sats_at:
            self.sats_at = now + 10.0
            self.sats = self.rand.randint(9, 11) if self.fix else min(3, int((now - self.start) / 2))
        fix_status = 0 if self.fix else 1
        if now >= self.next_fast:
            self.next_fast = now + 0.2
            self.sensor_value(0, int(round(self.speed * 10)), fix_status)
        if now >= self.next_slow:
            self.next_slow = now + 1.0
            self.sensor_value(1, int(self.heading) % 360, fix_status)
            self.sensor_value(2, self.altitude(), fix_status)
            self.sensor_value(3, self.sats, 0)
        if self.fix_text() != self.sent_fix:
            self.send_fix()
        if self.viewed(now) and now >= self.next_live:
            self.next_live = now + 0.5
            self.ui_value(G_SATS, self.sats)
            self.ui_value(G_SPEED, int(round(self.speed * 10)))

    def set_fix(self, on: bool) -> None:
        now = self.clock()
        if on and not self.fix:
            self.fix_at = now + 3.0
            self.log("searching: fix in 3 s")
        elif not on:
            self.fix, self.fix_at, self.sats, self.sats_at = False, None, 2, now + 1e9
            self.line("fix lost", 2)

    def command(self, words: list[str]) -> str | None:
        cmd = words[0] if words else "status"
        if cmd == "status":
            return self.status()
        if cmd == "fix" and len(words) > 1 and words[1] in ("on", "off"):
            if words[1] == "on":
                self.sats_at = 0.0
            self.set_fix(words[1] == "on")
            return ""
        return None

    def status(self) -> str:
        return (f"node 0x{self.node:02x} slot {self.slot}, {self.fix_text()}, {self.speed:.1f} km/h, "
                f"heading {self.heading:.0f}, altitude {self.altitude()} m, {self.distance / 1000:.2f} km driven")


# ==== the LTE gateway (docs/hardware/lte_card.md, can_channels.md Network gateway and ONLINE)

PLACES = [("Congress Avenue", "Austin", "Texas", 35), ("East Cesar Chavez Street", "Austin", "Texas", 30),
          ("South Lamar Boulevard", "Austin", "Texas", 40), ("Riverside Drive", "Austin", "Texas", 40),
          ("Ben White Boulevard", "Austin", "Texas", 55), ("South Congress Avenue", "Austin", "Texas", 35)]
TZ_NAME = "America/Chicago"
FIRST_MESSAGE = ("+1 512 555 0142", "Running 10 min late, save me a seat")
L_NETWORK, L_SIGNAL, L_DATA, L_CAP, L_LOOKUP = range(5)
PLACE_S, WEATHER_S, TZ_S, MESSAGE_S, RESEND_S = 30.0, 900.0, 300.0, 60.0, 30.0


def tz_now(utc: float) -> tuple[int, bool]:
    """(offset in 15-minute steps, daylight saving now) for TZ_NAME."""
    if ZoneInfo is not None:
        try:
            import datetime
            d = datetime.datetime.fromtimestamp(utc, ZoneInfo(TZ_NAME))
            off, dst = d.utcoffset(), d.dst()
            return int(off.total_seconds() // 900) if off else 0, bool(dst)
        except Exception:  # no tz database
            pass
    return -20, True


class Message:
    def __init__(self, mid: int, sender: str, text: str, at: int) -> None:
        self.id, self.sender, self.text, self.at = mid, sender, text, at
        self.sent_at = 0.0


class LteGateway(SimCard):
    CMD = "lte"
    NAME = "LTE (sim)"
    NODE_TYPE = NODE_TYPE_GATEWAY
    PRODUCT = PRODUCT_LTE
    UNIQUE_ID = 0x5D0A0006
    PRODUCES = ("ONLINE", "UI", "LOG")
    CONSUMES = ("ONLINE",)
    LISTENS = (CH_UI, CH_ONLINE)
    HELP = "status | message [text] | refresh | offline | online"

    def __init__(self, send: Send, log: Log, slot: int = 4, seed: int | None = None, clock: Now | None = None,
                 speed_kmh: Callable[[], float] | None = None, wall: Callable[[], float] | None = None) -> None:
        super().__init__(send, log, slot=slot, seed=seed, clock=clock)
        self.speed_kmh = speed_kmh or (lambda: 0.0)
        self.wall = wall or time.time
        self.online = True
        self.rssi = -79
        self.data_kb = 412 * 1024.0
        self.cap_mb = 0
        self.place = -1                    # index into PLACES, -1 = not looked up yet
        self.serial = 0
        self.moved = 0.0                   # metres since the last place lookup
        self.place_at: float | None = None
        self.lookups: list[float] = []
        self.weather_valid = self.tz_valid = False
        self.last_error = 0
        self.next_place = self.next_weather = self.next_tz = 0.0
        self.next_gw = self.next_online = self.next_live = 0.0
        self.message_at: float | None = self.start + MESSAGE_S
        self.messages: list[Message] = []
        self.next_id = 1
        self.sent_gw: tuple[int, ...] | None = None
        self.last = self.start

    # ---- the uplink

    def uplink(self, now: float) -> int:
        up = now - self.start
        if up < 2.0:
            return 1
        if up < 3.0 or not self.online:
            return 2
        return 3

    def bars(self) -> int:
        return max(0, min(5, (self.rssi + 113) // 8))

    def data_mb(self) -> int:
        return int(self.data_kb // 1024)

    def gw_tuple(self, now: float) -> tuple[int, ...]:
        flags = 4 if self.cap_mb and self.data_mb() >= self.cap_mb else 0
        up = self.uplink(now)
        return up, self.bars() if up > 1 else 0, self.rssi if up > 1 else -113, 3 if up > 1 else 0, 0, \
            min(self.data_mb(), 0xFFFF), 0, flags

    def send_gateway_status(self, now: float) -> None:
        self.sent_gw = self.gw_tuple(now)
        self.transfer(CH_NET, struct.pack("<BBBbBBHBB", NET_GATEWAY_STATUS, *self.sent_gw))

    def network_text(self, now: float) -> str:
        up = self.uplink(now)
        return ("off", "Searching", "LTE, registered, no data", f"LTE, {self.bars()} bars, data up")[up]

    # ---- ONLINE

    def lookup_place(self, now: float, force: bool = False) -> None:
        """The street lookup (Nominatim): a new place once the car has moved 50 m (and 10 s passed), or on boot."""
        if not self.online:
            self.last_error = 2
            self.send_place(now)
            return
        if self.place < 0 or self.moved >= 50 or force:
            if self.place < 0 or self.moved >= 50:
                self.place = (self.place + 1) % len(PLACES)
                self.serial = (self.serial + 1) & 0xFF
            self.moved = 0.0
            self.place_at = now
            self.lookups.append(now)
            self.data_kb += 1
            self.last_error = 0
            self.log(f"place: {PLACES[self.place][0]}, {PLACES[self.place][1]}")
        self.send_place(now)

    def send_place(self, now: float) -> None:
        self.next_place = now + PLACE_S
        if self.place < 0:
            return
        street, locality, region, limit = PLACES[self.place]
        flags = 1 | (0 if self.online else 2) | 4
        self.transfer(CH_ONLINE, struct.pack("<BBBB", 0x01, self.serial, flags, limit) + b"US" + lp(street, 48)
                      + lp(locality, 32) + lp(region, 32))

    def send_weather(self, now: float) -> None:
        self.next_weather = now + WEATHER_S
        if not self.online:
            self.last_error = 2
            return
        utc = int(self.wall())
        off, _ = tz_now(utc)
        midnight = (utc + off * 900) // 86400 * 86400 - off * 900
        temp = 234 + self.rand.randint(-6, 6)
        self.transfer(CH_ONLINE, struct.pack("<BIhhBBHHBhhII", 0x02, utc, temp, temp + 7, 2, 58, 36, 160, 10, 278, 162,
                                             midnight + 7 * 3600 + 24 * 60, midnight + 19 * 3600 + 12 * 60))
        self.weather_valid = True
        self.data_kb += 1
        self.log(f"weather: {temp / 10:.1f} C, partly cloudy")

    def send_timezone(self, now: float) -> None:
        self.next_tz = now + TZ_S
        if not self.online and not self.tz_valid:
            return
        off, dst = tz_now(self.wall())
        self.transfer(CH_ONLINE, struct.pack("<Bbb", 0x03, off, 1 if dst else 0) + lp(TZ_NAME, 64))
        self.tz_valid = True

    def send_online_status(self, now: float) -> None:
        self.lookups = [t for t in self.lookups if now - t < 3600]
        flags = (1 if self.uplink(now) == 3 else 0) | (2 if self.place >= 0 else 0) | \
            (4 if self.weather_valid else 0) | (8 if self.tz_valid else 0)
        since = 0xFFFF if self.place_at is None else min(int(now - self.place_at), 0xFFFE)
        self.transfer(CH_ONLINE, struct.pack("<BBHBB", 0x05, flags, since, min(len(self.lookups), 255),
                                             self.last_error))

    def add_message(self, sender: str, text: str) -> Message:
        m = Message(self.next_id, sender, text, int(self.wall()))
        self.next_id = self.next_id % 255 + 1
        self.messages.append(m)
        self.line(f"SMS {m.id} from {sender}: {text}")
        return m

    def send_message(self, m: Message, now: float) -> None:
        m.sent_at = now
        self.transfer(CH_ONLINE, struct.pack("<BBBI", 0x04, m.id, 1, m.at) + lp(m.sender, 32) + lp(m.text, 160))

    # ---- UI page

    def items(self) -> None:
        now = self.clock()
        self.item(L_NETWORK, TOP, K_TEXT, F_RO, "Network")
        self.item(L_SIGNAL, TOP, K_READOUT, F_RO | F_LIVE, "Signal", "dBm")
        self.item(L_DATA, TOP, K_READOUT, F_RO, "Data this month", "MB")
        self.item(L_CAP, TOP, K_NUMBER, F_SETTING, "Data cap", "MB", lo=0, hi=50000, step=100)
        self.item(L_LOOKUP, TOP, K_ACTION, 0, "Look up now")
        self.ui_text(L_NETWORK, self.network_text(now))
        for n in (L_SIGNAL, L_DATA, L_CAP):
            self.send_value(n)

    def send_value(self, n: int) -> None:
        v = {L_SIGNAL: self.rssi, L_DATA: self.data_mb(), L_CAP: self.cap_mb, L_LOOKUP: 0}.get(n)
        if v is not None:
            self.ui_value(n, v)

    def set_cap(self, mb: int) -> None:
        self.cap_mb = mb
        self.factory = False
        self.log(f"data cap {mb} MB" if mb else "no data cap")

    def refresh(self, what: int, now: float) -> None:
        if what & 1:
            self.lookup_place(now, force=True)
        if what & 2:
            self.send_weather(now)
        if what & 4:
            self.send_timezone(now)

    # ---- receiving

    def receive(self, base: int, src: int, op: int, p: bytes, now: float) -> None:
        if base == CH_UI and op == 0x81 and len(p) >= 7:
            n, v = p[2], struct.unpack_from("<i", p, 3)[0]
            if n in (L_NETWORK, L_SIGNAL, L_DATA):
                result = UI_READONLY
            elif n == L_CAP:
                result = UI_OK if 0 <= v <= 50000 else UI_RANGE
                if result == UI_OK:
                    self.set_cap(v)
            elif n == L_LOOKUP:
                result = UI_OK if v == 1 else UI_RANGE
                if v == 1:
                    self.refresh(7, now)
            else:
                result = UI_UNKNOWN
            self.send_value(n)
            self.ui_result(n, result, src)
        elif base == CH_ONLINE and op == 0x81 and len(p) >= 3 and p[1] == self.node:
            self.log(f"REFRESH 0x{p[2]:x} from 0x{src:02x}")
            self.refresh(p[2], now)
        elif base == CH_ONLINE and op == 0x82 and len(p) >= 3 and p[1] == self.node:
            hit = [m for m in self.messages if m.id == p[2]]
            self.messages = [m for m in self.messages if m.id != p[2]]
            if hit:
                self.log(f"message {p[2]} read on 0x{src:02x}")
        elif base == CH_NET and op == NET_GATEWAY_CMD and len(p) >= 5 and p[1] == self.node:
            if p[2] == 4:
                self.set_cap(struct.unpack_from("<H", p, 3)[0])
                self.send_value(L_CAP)
            else:
                self.log(f"GATEWAY CMD {p[2]} ignored (reserved)")

    # ---- time

    def periodic(self, now: float) -> None:
        dt = min(max(now - self.last, 0.0), 1.0)
        self.last = now
        kmh = max(0.0, self.speed_kmh())
        self.moved += kmh / 3.6 * dt
        if self.uplink(now) == 3 and kmh > 1:
            self.data_kb += 0.4 * dt
        if now - self.start >= 3.0:
            if self.place < 0 and self.online:
                self.lookup_place(now)
                self.send_weather(now)
                self.send_timezone(now)
            elif self.moved >= 50 and self.place_at is not None and now - self.place_at >= 10 and self.online:
                self.lookup_place(now)
            if now >= self.next_place:
                self.send_place(now)
            if now >= self.next_weather:
                self.send_weather(now)
            if now >= self.next_tz:
                self.send_timezone(now)
        if self.message_at is not None and now >= self.message_at:
            self.message_at = None
            self.add_message(*FIRST_MESSAGE)
        for m in self.messages:
            if self.online and (not m.sent_at or now - m.sent_at >= RESEND_S):
                self.send_message(m, now)
        if now >= self.next_gw or self.gw_tuple(now) != self.sent_gw:
            if now >= self.next_gw:
                self.rssi = -79 + self.rand.randint(-3, 3)
            self.next_gw = now + 1.0
            self.send_gateway_status(now)
        if now >= self.next_online:
            self.next_online = now + 1.0
            self.send_online_status(now)
        if self.viewed(now) and now >= self.next_live:
            self.next_live = now + 1.0
            self.ui_text(L_NETWORK, self.network_text(now))
            self.send_value(L_SIGNAL)
            self.send_value(L_DATA)

    def boot_text(self) -> str:
        return f"simulated LTE gateway at node 0x{self.node:02x} (slot {self.slot}), data up in 3 s"

    def command(self, words: list[str]) -> str | None:
        cmd = words[0] if words else "status"
        now = self.clock()
        if cmd == "status":
            return self.status()
        if cmd == "message":
            self.add_message(FIRST_MESSAGE[0], " ".join(words[1:]) or "Where are you parked?")
        elif cmd == "refresh":
            self.refresh(7, now)
        elif cmd in ("offline", "online"):
            self.online = cmd == "online"
            self.last_error = 0 if self.online else 2
            self.line(f"data connection {'up' if self.online else 'lost'}", 1 if self.online else 2)
            self.lookup_place(now)  # online: a fresh lookup; offline: the cached place, flag bit1
        else:
            return None
        return ""

    def status(self) -> str:
        now = self.clock()
        place = f"{PLACES[self.place][0]}, {PLACES[self.place][1]} (serial {self.serial})" if self.place >= 0 else "none"
        return (f"node 0x{self.node:02x} slot {self.slot}, {self.network_text(now)}, {self.rssi} dBm, "
                f"{self.data_mb()} MB this month{f' of {self.cap_mb}' if self.cap_mb else ''}, place {place}, "
                f"{len(self.messages)} unread message{'s' if len(self.messages) != 1 else ''}")


# ==== the AI voice card

SPEAK_S = 3.0


class VoiceCard(SimCard):
    CMD = "voice"
    NAME = "Voice (sim)"
    LABEL = "AI voice card"
    NODE_TYPE = NODE_TYPE_VOICE
    PRODUCT = PRODUCT_VOICE
    UNIQUE_ID = 0x5D0A0007
    PRODUCES = ("LOG",)
    HELP = "status | say [text]"

    def __init__(self, send: Send, log: Log, slot: int = 5, seed: int | None = None, clock: Now | None = None) -> None:
        super().__init__(send, log, slot=slot, seed=seed, clock=clock)
        self.sink_node = 0
        self.state = "idle"          # idle, asking, speaking
        self.text = ""
        self.until = 0.0

    def heard(self, src: int, ntype: int, product: int) -> None:
        if ntype == NODE_TYPE_SINK and product == PRODUCT_AUDIO_CARD and self.sink_node != src:
            self.sink_node = src
            self.log(f"audio card heard at 0x{src:02x}: listening on MIC for the wake word")

    def say(self, text: str) -> str:
        if not self.sink_node:
            return "no audio card heard: nothing to ask for RET"
        if self.state != "idle":
            return f"busy ({self.state})"
        self.text = text
        self.state = "asking"
        self.rail_send(self.sink_node, RAIL_REQUEST)
        self.log(f"asks the audio card 0x{self.sink_node:02x} for RET")
        return ""

    def on_rail(self, src: int, p: bytes, now: float) -> None:
        if src != self.sink_node:
            return
        if p[3] == RAIL_GRANTED and self.state == "asking":
            self.state, self.until = "speaking", now + SPEAK_S
            self.line(f'speaks: "{self.text}"')
        elif p[3] == RAIL_RELEASE:
            was = self.state
            self.state = "idle"
            if was == "speaking":
                self.log("RET withdrawn: stops speaking within 10 ms")
            elif was == "asking":
                self.log("RET denied")

    def periodic(self, now: float) -> None:
        if self.state != "speaking":
            return
        if now - self.heard_at.get(self.sink_node, now) > HOLDER_LOST_S:
            self.state = "idle"
            self.log("audio card's heartbeat lost: stops driving RET at once")
        elif now >= self.until:
            self.state = "idle"
            self.rail_send(self.sink_node, RAIL_RELEASE)
            self.log("done speaking: releases RET")

    def command(self, words: list[str]) -> str | None:
        cmd = words[0] if words else "status"
        if cmd == "status":
            return self.status()
        if cmd == "say":
            return self.say(" ".join(words[1:]) or "Turn left in 300 metres onto Congress Avenue")
        return None

    def status(self) -> str:
        sink = f"0x{self.sink_node:02x}" if self.sink_node else "not heard"
        return f"node 0x{self.node:02x} slot {self.slot}, {self.state}, audio card {sink}"


# ==== the transmission-temperature card (can_channels.md, Examples)

T_CAUTION, T_WARNING = 1100, 1250       # 0.1 °C, the example's thresholds
T_CLEAR = 50                            # alerts clear 5 °C under their threshold
T_OFFSET = 0
TAU_S = 4.0


class TransTempCard(SimCard):
    CMD = "trans"
    NAME = "Trans temp"
    LABEL = "transmission-temperature card"
    NODE_TYPE = NODE_TYPE_SENSOR
    PRODUCT = PRODUCT_NONE
    UNIQUE_ID = 0x5D0A0100
    PRODUCES = ("SENSOR", "ALERT", "UI")
    CONSUMES = ("SYSTEM",)
    LISTENS = (CH_UI, CH_ALERT, CH_SYSTEM)
    HELP = "status | temp <deg C> | mode own|center"

    def __init__(self, send: Send, log: Log, slot: int = 6, seed: int | None = None, clock: Now | None = None) -> None:
        super().__init__(send, log, slot=slot, seed=seed, clock=clock)
        self.temp = 82.0                # °C, what the thermistor reads
        self.target = 82.0
        self.offset = T_OFFSET          # 0.1 °C, UI item 0
        self.own_alerts = True          # False: the example's center-raised alerts (SENSOR flag bit2) instead
        self.level = 0                  # 0 none, 1 caution, 2 warning
        self.next_value = self.next_state = 0.0
        self.last = self.start

    def value(self) -> int:
        return int(round(self.temp * 10)) + self.offset

    def active(self) -> int:
        if not self.own_alerts:
            return 0
        return (1, 2)[self.level - 1] if self.level else 0

    def sensor_defines(self) -> None:
        flags = 0x03 if self.own_alerts else 0x07
        self.sensor_define(0, 1, -1, flags, 200, 400, 1500, "Trans temp", caution=(-0x80000000, T_CAUTION),
                           warning=(-0x80000000, T_WARNING))

    def alert_defines(self) -> None:
        if not self.own_alerts:
            return
        self.transfer(CH_ALERT, struct.pack("<BBBBBH", 0x01, 0, 2, 0x01, 1, 0) + b"Trans fluid hot")
        self.transfer(CH_ALERT, struct.pack("<BBBBBH", 0x01, 1, 3, 0x01, 1, 0) + b"Trans fluid overheating")

    def send_state(self) -> None:
        self.transfer(CH_ALERT, struct.pack("<BIB", 0x02, self.active(), self.generation))

    def items(self) -> None:
        self.item(0, TOP, K_NUMBER, F_SETTING, "Offset", "°C", exp=-1, lo=-50, hi=50, step=1)
        self.ui_value(0, self.offset)

    def evaluate(self) -> None:
        v = self.value()
        if v >= T_WARNING or (self.level == 2 and v >= T_WARNING - T_CLEAR):
            level = 2
        elif v >= T_CAUTION or (self.level >= 1 and v >= T_CAUTION - T_CLEAR):
            level = 1
        else:
            level = 0
        if level != self.level:
            self.level = level
            self.log(f"{v / 10:.1f} C: " + ("clear", "caution (Trans fluid hot)",
                                            "warning (Trans fluid overheating)")[level])
            if self.own_alerts:
                self.send_state()
                self.next_state = self.clock() + 1.0

    def receive(self, base: int, src: int, op: int, p: bytes, now: float) -> None:
        if base == CH_UI and op == 0x81 and len(p) >= 7:
            n, v = p[2], struct.unpack_from("<i", p, 3)[0]
            if n != 0:
                result = UI_UNKNOWN
            elif not -50 <= v <= 50:
                result = UI_RANGE
            else:
                self.offset, self.factory, result = v, False, UI_OK
            if n == 0:
                self.ui_value(0, self.offset)
            self.ui_result(n, result, src)
        elif base == CH_ALERT and op == 0x81 and len(p) >= 3 and p[1] == self.node:
            self.log(f"alert {p[2]} acknowledged by 0x{src:02x} (its own buzzer would stop)")
        elif base == CH_SYSTEM and op == 0x04 and len(p) >= 2 and p[1] == 1:
            self.log("ignition off: offset already saved")

    def periodic(self, now: float) -> None:
        dt = min(max(now - self.last, 0.0), 1.0)
        self.last = now
        self.temp += (self.target - self.temp) * (1 - math.exp(-dt / TAU_S))
        if abs(self.target - self.temp) < 0.05:
            self.temp = self.target
        self.evaluate()
        if now >= self.next_value:
            self.next_value = now + 0.2
            self.sensor_value(0, self.value(), 0)
        if self.own_alerts and now >= self.next_state:
            self.next_state = now + 1.0
            self.send_state()

    def set_mode(self, own: bool) -> None:
        if own == self.own_alerts:
            return
        if self.own_alerts and self.level:
            self.own_alerts = own
            self.send_state()  # clears its own alerts (active 0) before the center takes over
        self.own_alerts = own
        self.generation = self.generation % 255 + 1
        self.announce()
        self.sensor_defines()
        self.alert_defines()
        if own:
            self.send_state()
        self.log(f"alerts {'its own (ALERT 0 / 1)' if own else 'raised by the center from the thresholds'}")

    def command(self, words: list[str]) -> str | None:
        cmd = words[0] if words else "status"
        if cmd == "status":
            return self.status()
        if cmd == "temp" and len(words) > 1:
            try:
                self.target = float(words[1])
            except ValueError:
                return None
            return f"heading for {self.target:.1f} C (caution {T_CAUTION / 10:.0f}, warning {T_WARNING / 10:.0f})"
        if cmd == "mode" and len(words) > 1 and words[1] in ("own", "center"):
            self.set_mode(words[1] == "own")
            return ""
        return None

    def status(self) -> str:
        return (f"node 0x{self.node:02x} slot {self.slot}, {self.value() / 10:.1f} C (target {self.target:.1f}, "
                f"offset {self.offset / 10:+.1f}), {('no alert', 'caution', 'warning')[self.level]}, "
                f"alerts {'own' if self.own_alerts else 'from the center'}")


CARDS = (AudioCard, GpsCard, LteGateway, VoiceCard, TransTempCard)


# ==== tests (no hardware)

def _ann(ntype: int, product: int, name: str) -> bytes:
    return struct.pack("<BBBHHIBIIB", 0x01, 1, ntype, product, 1, 7, 1, 0, 0, 0) + name.encode()


def _test_audio() -> None:
    clock = Clock()
    bus = Bus(clock)
    a = AudioCard(bus.send, bus.lines.append, clock=clock)
    sources: list[bool] = []
    a.on_source = sources.append
    a.boot()
    a.tick()
    ann = bus.got(CH_NET, 0x01)[0]
    produces, consumes = struct.unpack_from("<II", ann, 12)
    assert ann[2] == 5 and struct.unpack_from("<H", ann, 3)[0] == 4 and ann[20] == 1 | 1 << 1
    assert produces == (1 << 18) | (1 << 19) | (1 << 31) | (1 << 30) and consumes == (1 << 18) | (1 << 2) | (1 << 30)
    st = bus.got(CH_AUDIO_CMD, 0x01)
    assert st and st[-1] == bytes([0x01, 30, 0, 0, 0, 0x0F])
    deliver(a, 0x15, CH_NET, _ann(NODE_TYPE_RADIO, 9, "Radio (sim)"), 0)
    deliver(a, 0x18, CH_NET, _ann(NODE_TYPE_VOICE, PRODUCT_VOICE, "Voice (sim)"), 0)
    deliver(a, 0x17, CH_NET, _ann(NODE_TYPE_GATEWAY, PRODUCT_LTE, "LTE (sim)"), 0)
    # sounds: a caution chime ducks the music and ends with DONE
    bus.clear()
    deliver(a, 0x01, CH_AUDIO_CMD, struct.pack("<BBHBBB", 0x81, 0x14, 3, 0x03, 100, 1), 1)
    a.tick()
    assert bus.got(CH_AUDIO_CMD, 0x01)[-1][2] & 4 and bus.got(CH_AUDIO_CMD, 0x01)[-1][3] == 1
    clock.t += 1.6
    a.tick()
    assert bus.got(CH_AUDIO_CMD, 0x02) == [struct.pack("<BBH", 0x02, 0x01, 3)]
    assert not a.sounds and not bus.got(CH_AUDIO_CMD, 0x01)[-1][2] & 4
    # a parking tick until STOP; a warning chime in the same zones interrupts it
    deliver(a, 0x19, CH_AUDIO_CMD, struct.pack("<BBHBBB", 0x81, 0x00, 7, 0x31, 80, 0), 2)
    clock.t += 5
    a.tick()
    assert (0x19, 7) in a.sounds
    bus.clear()
    deliver(a, 0x01, CH_AUDIO_CMD, struct.pack("<BBHBBB", 0x81, 0x14, 4, 0x04, 100, 1), 3)
    assert (0x19, 7) not in a.sounds and bus.got(CH_AUDIO_CMD, 0x02) == [struct.pack("<BBH", 0x02, 0x19, 7)]
    deliver(a, 0x01, CH_AUDIO_CMD, struct.pack("<BBH", 0x82, 0x14, 0xFFFF), 4)
    assert not a.sounds
    # volume: VOLUME, keys, encoder 1 (cumulative, wraps)
    deliver(a, 0x13, CH_AUDIO_CMD, struct.pack("<BBBb", 0x83, 0x00, 0, 50), 5)
    assert a.volume == 50
    deliver(a, 0x13, CH_INPUT, bytes([0x01, KEY_VOL_UP, G_CLICK, 1]), 0)
    assert a.volume == 52
    deliver(a, 0x13, CH_INPUT, bytes([0x01, KEY_VOL_UP, G_CLICK, 1]), 1)
    assert a.volume == 52                                       # same sequence: a retried duplicate
    deliver(a, 0x13, CH_INPUT, struct.pack("<BBh", 0x02, 1, 32766), 2)
    deliver(a, 0x13, CH_INPUT, struct.pack("<BBh", 0x02, 1, -32766), 3)
    assert a.volume == 56, a.volume                            # +4 across the wrap
    deliver(a, 0x13, CH_INPUT, bytes([0x01, KEY_MUTE, G_CLICK, 2]), 4)
    a.tick()
    assert a.muted and bus.got(CH_AUDIO_CMD, 0x01)[-1][2] & 1
    # media keys: Bluetooth here, forwarded to the radio with Source = Radio
    deliver(a, 0x13, CH_INPUT, bytes([0x01, KEY_PLAY, G_CLICK, 3]), 5)
    assert a.bt_playing
    deliver(a, 0x13, CH_INPUT, bytes([0x01, KEY_NEXT, G_CLICK, 4]), 6)
    assert a.track == 1
    bus.clear()
    deliver(a, 0x01, CH_UI, struct.pack("<BBBi", 0x81, 0x14, A_SOURCE, 1), 0)
    assert a.source == 1 and sources == [True] and bus.got(CH_UI, 0x04) == [bytes([0x04, 0, 0, 0x01])]
    deliver(a, 0x13, CH_INPUT, bytes([0x01, KEY_PREV, G_CLICK, 5]), 7)
    assert bus.got(CH_AUDIO_CMD, 0x84) == [bytes([0x84, 0x15, MEDIA_PREV])] and a.track == 1
    deliver(a, 0x01, CH_UI, struct.pack("<BBBi", 0x81, 0x14, A_FADER, 11), 1)
    assert bus.got(CH_UI, 0x04)[-1] == bytes([0x04, A_FADER, UI_RANGE, 0x01])
    # RAIL: the radio gets RET, the gateway is denied, the voice card takes it, its heartbeat stops, RET frees
    bus.clear()
    a.on_frame(HEARTBEAT + 0x15, b"\x09\x01\x00\x00\x00\x00\x00\x01")
    deliver(a, 0x15, CH_NET, struct.pack("<BBBBH", 0x84, 0x14, 2, 0, 4800), 1)
    assert a.holder == 0x15 and bus.got(CH_NET, 0x84)[-1] == struct.pack("<BBBBH", 0x84, 0x15, 1, 1, 4800)
    a.tick()
    assert bus.got(CH_AUDIO_CMD, 0x01)[-1][2] & 2               # the radio is the source and holds RET: playing
    deliver(a, 0x18, CH_NET, struct.pack("<BBBBH", 0x84, 0x14, 5, 0, 4800), 1)
    rails = bus.got(CH_NET, 0x84)
    assert a.holder == 0x18 and rails[-2][1:4:2] == bytes([0x15, 2]) and rails[-1][1:4:2] == bytes([0x18, 1])
    deliver(a, 0x17, CH_NET, struct.pack("<BBBBH", 0x84, 0x14, 4, 0, 4800), 1)
    assert a.holder == 0x18 and bus.got(CH_NET, 0x84)[-1][1] == 0x17 and bus.got(CH_NET, 0x84)[-1][3] == 2
    a.tick()
    assert bus.got(CH_AUDIO_CMD, 0x01)[-1][2] & 4               # voice ducks the music
    clock.t += 3.5
    a.tick()
    assert a.holder == 0                                        # no heartbeat from 0x18 for 3 s
    deliver(a, 0x15, CH_NET, struct.pack("<BBBBH", 0x84, 0x14, 2, 0, 4800), 2)
    deliver(a, 0x15, CH_NET, struct.pack("<BBBBH", 0x84, 0x14, 2, 2, 4800), 3)
    assert a.holder == 0
    print("cards_sim: audio card ok")


def _test_gps() -> None:
    clock = Clock()
    bus = Bus(clock)
    speed = [50.0]
    g = GpsCard(bus.send, bus.lines.append, clock=clock, speed_kmh=lambda: speed[0])
    assert g.node == 0x16
    g.boot()
    g.tick()
    defs = bus.got(CH_SENSOR, 0x01)
    assert [d[1] for d in defs] == [0, 1, 2, 3] and defs[0][2] == 6 and defs[0][3] == 0xFF  # km/h, exponent -1
    vals = bus.got(CH_SENSOR, 0x02)
    assert vals[0][:2] == b"\x02\x00" and vals[0][6] == 1      # no fix yet: not ready
    assert bus.got(CH_UI, 0x03)[-1].startswith(b"\x03\x00Searching")
    for _ in range(40):
        clock.t += 0.2
        g.tick()
    assert g.fix and bus.got(CH_UI, 0x03)[-1].startswith(b"\x03\x003D fix")
    v = bus.got(CH_SENSOR, 0x02)[-1]
    sp = [p for p in bus.got(CH_SENSOR, 0x02) if p[1] == 0][-1]
    assert struct.unpack_from("<iB", sp, 2) == (500, 0) and v
    assert 9 <= g.sats <= 11 and g.distance > 0
    bus.clear()
    deliver(g, 0x01, CH_UI, bytes([0x82, 0x16, 0xFE]), 0)
    clock.t += 0.6
    g.tick()
    assert bus.got(CH_UI, 0x02)                                 # live readouts while viewed
    g.set_fix(False)
    clock.t += 0.3
    g.tick()
    assert [p for p in bus.got(CH_SENSOR, 0x02) if p[1] == 0][-1][6] == 1
    print("cards_sim: GPS card ok")


def _test_lte() -> None:
    clock = Clock()
    bus = Bus(clock)
    speed = [0.0]
    w = [1790000000.0]  # 2026-09-21, Chicago on daylight saving time
    lte = LteGateway(bus.send, bus.lines.append, clock=clock, speed_kmh=lambda: speed[0], wall=lambda: w[0])
    lte.boot()
    lte.tick()
    gs = bus.got(CH_NET, NET_GATEWAY_STATUS)
    assert gs and gs[-1][1] == 1 and len(gs[-1]) == 10          # searching
    for _ in range(40):
        clock.t += 0.1
        lte.tick()
    assert bus.got(CH_NET, NET_GATEWAY_STATUS)[-1][1] == 3
    place = bus.got(CH_ONLINE, 0x01)
    assert len(place) == 1 and place[0][1] == 1 and place[0][2] == 0x05 and place[0][4:6] == b"US"
    assert place[0][7:7 + place[0][6]] == b"Congress Avenue"
    weather = bus.got(CH_ONLINE, 0x02)
    assert len(weather) == 1 and len(weather[0]) == 28
    tz = bus.got(CH_ONLINE, 0x03)
    assert tz == [struct.pack("<Bbb", 0x03, -20, 1) + lp(TZ_NAME, 64)] or ZoneInfo is None, tz
    st = bus.got(CH_ONLINE, 0x05)[-1]
    assert st[1] == 0x0F and st[4] == 1 and st[5] == 0
    # 30 s later PLACE repeats with the same serial; driving 60 m, more than 10 s later, gives a new place
    for _ in range(31):
        clock.t += 1.0
        lte.tick()
    assert [p[1] for p in bus.got(CH_ONLINE, 0x01)] == [1, 1]
    speed[0] = 36.0
    for _ in range(7):
        clock.t += 1.0
        lte.tick()
    assert bus.got(CH_ONLINE, 0x01)[-1][1] == 2 and lte.place == 1
    # the message at 60 s, again after 30 s, until MESSAGE READ
    speed[0] = 0.0
    for _ in range(30):
        clock.t += 1.0
        lte.tick()
    msgs = bus.got(CH_ONLINE, 0x04)
    assert len(msgs) == 1 and msgs[0][1] == 1 and msgs[0][2] == 1
    n = msgs[0][7]
    assert msgs[0][8:8 + n] == FIRST_MESSAGE[0].encode()
    for _ in range(31):
        clock.t += 1.0
        lte.tick()
    assert len(bus.got(CH_ONLINE, 0x04)) == 2
    deliver(lte, 0x01, CH_ONLINE, bytes([0x82, lte.node, 1]), 0)
    assert not lte.messages
    # REFRESH weather, GATEWAY CMD data cap, offline
    bus.clear()
    deliver(lte, 0x02, CH_ONLINE, bytes([0x81, lte.node, 2]), 0)
    assert len(bus.got(CH_ONLINE, 0x02)) == 1
    deliver(lte, 0x01, CH_NET, struct.pack("<BBBH", 0x8B, lte.node, 4, 300), 0)
    assert lte.cap_mb == 300
    lte.tick()
    assert bus.got(CH_NET, NET_GATEWAY_STATUS)[-1][9] & 4      # 412 MB used: over the cap
    lte.command(["offline"])
    clock.t += 1.0
    lte.tick()
    assert bus.got(CH_NET, NET_GATEWAY_STATUS)[-1][1] == 2 and bus.got(CH_ONLINE, 0x05)[-1][5] == 2
    assert bus.got(CH_ONLINE, 0x01)[-1][2] & 2                  # cached place
    print("cards_sim: LTE gateway ok")


def _test_voice() -> None:
    clock = Clock()
    bus = Bus(clock)
    v = VoiceCard(bus.send, bus.lines.append, clock=clock)
    assert v.node == 0x18
    v.boot()
    assert v.say("hello")
    deliver(v, 0x14, CH_NET, _ann(NODE_TYPE_SINK, PRODUCT_AUDIO_CARD, "Audio"), 0)
    assert v.say("hello") == "" and bus.got(CH_NET, 0x84)[-1] == struct.pack("<BBBBH", 0x84, 0x14, 5, 0, 4800)
    v.on_frame(HEARTBEAT + 0x14, b"\x05\x01\x00\x00\x00\x00\x00\x01")
    deliver(v, 0x14, CH_NET, struct.pack("<BBBBH", 0x84, 0x18, 1, 1, 4800), 1)
    assert v.state == "speaking"
    clock.t += 2.0
    v.on_frame(HEARTBEAT + 0x14, b"\x05\x01\x00\x00\x00\x00\x00\x01")
    v.tick()
    assert v.state == "speaking"
    clock.t += 1.1
    v.tick()
    assert v.state == "idle" and bus.got(CH_NET, 0x84)[-1][3] == 2
    v.say("again")
    deliver(v, 0x14, CH_NET, struct.pack("<BBBBH", 0x84, 0x18, 1, 2, 4800), 2)
    assert v.state == "idle"                                    # denied
    print("cards_sim: voice card ok")


def _test_trans() -> None:
    clock = Clock()
    bus = Bus(clock)
    t = TransTempCard(bus.send, bus.lines.append, slot=1, clock=clock)   # the example's slot: node 0x14
    t.boot()
    ann = bus.got(CH_NET, 0x01)[0]
    produces, consumes = struct.unpack_from("<II", ann, 12)
    assert ann[2] == 3 and ann[21:] == b"Trans temp"
    assert produces == (1 << 9) | (1 << 1) | (1 << 19) | (1 << 30) and consumes == (1 << 3) | (1 << 30)
    # the example's frames, byte for byte (transfer IDs as in the example)
    t.temp = t.target = 104.3
    t.tids[CH_SENSOR + 0x14] = 7
    t.sensor_value(0, t.value(), 0)
    assert bus.frames[-1] == (0x134, bytes.fromhex("02001304000000C7"))
    bus.clear()
    t.tids[CH_ALERT + 0x14] = 3
    t.alert_defines()
    assert bus.frames[:4] == [(0x034, bytes.fromhex(h)) for h in ("5FC1010002010183", "00005472616E7323",
                                                                   "20666C7569642003", "686F7463")]
    t.level = 1
    t.tids[CH_ALERT + 0x14] = 12
    t.send_state()
    assert bus.frames[-1] == (0x034, bytes.fromhex("020100000001CC"))
    t.level = 0
    t.sensor_defines()
    sd = bus.got(CH_SENSOR, 0x01)[-1]
    assert struct.unpack_from("<BBBbBHiiiiii", sd) == (1, 0, 1, -1, 0x03, 200, 400, 1500, -0x80000000, 1100,
                                                       -0x80000000, 1250) and sd[31:] == b"Trans temp"
    t.items()
    item = bus.got(CH_UI, 0x01)
    assert struct.unpack_from("<BBBBBbiii", item[0]) == (1, 0, 0xFF, 3, 0x10, -1, -50, 50, 1)
    # heat up past caution and warning, then cool down
    t.command(["temp", "130"])
    seen = set()
    for _ in range(300):
        clock.t += 0.1
        t.tick()
        seen.add(t.level)
    assert seen == {0, 1, 2} and t.level == 2
    states = bus.got(CH_ALERT, 0x02)
    assert states[-1] == bytes([0x02, 2, 0, 0, 0, 1])
    t.command(["temp", "90"])
    for _ in range(300):
        clock.t += 0.1
        t.tick()
    assert t.level == 0 and bus.got(CH_ALERT, 0x02)[-1] == bytes([0x02, 0, 0, 0, 0, 1])
    # the center mode: SENSOR flag bit2 (center-alerts), no ALERT STATE
    t.command(["mode", "center"])
    sd = bus.got(CH_SENSOR, 0x01)[-1]
    assert sd[4] == 0x07 and t.generation == 2
    bus.clear()
    clock.t += 2
    t.tick()
    assert not bus.got(CH_ALERT, 0x02)
    deliver(t, 0x01, CH_UI, struct.pack("<BBBi", 0x81, 0x14, 0, 25), 0)
    assert t.offset == 25 and bus.got(CH_UI, 0x04)[-1] == bytes([0x04, 0, 0, 0x01])
    print("cards_sim: trans temp card ok")


def _test() -> None:
    _test_audio()
    _test_gps()
    _test_lte()
    _test_voice()
    _test_trans()
    print("cards_sim: all tests passed")


if __name__ == "__main__":
    import sys
    if "--test" in sys.argv:
        _test()
    else:
        print(__doc__)
