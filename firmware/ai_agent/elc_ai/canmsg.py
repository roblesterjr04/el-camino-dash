"""The CAN channel messages the assistant reads and sends (docs/protocols/can_channels.md). Mirrors firmware/elc_can's
elc_chan_msg.cpp; the layouts are the protocol's, so a change there is a change here."""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Optional, Union

CH_ALERT, CH_INPUT, CH_SYSTEM, CH_SENSOR = 0x020, 0x040, 0x060, 0x120
CH_AUDIO_CMD, CH_UI, CH_ONLINE, CH_NET, CH_LOG = 0x240, 0x260, 0x360, 0x3C0, 0x3E0

# quantities (registry)
Q_UNITLESS, Q_TEMP, Q_PRESSURE, Q_VOLTAGE, Q_CURRENT, Q_RPM, Q_SPEED, Q_PERCENT = range(8)
Q_LAMBDA, Q_AFR, Q_DISTANCE, Q_VOLUME, Q_FLOW, Q_ANGLE, Q_SWITCH, Q_COUNT = range(8, 16)
Q_FREQ, Q_ACCEL, Q_HUMIDITY, Q_DURATION, Q_ECONOMY = range(16, 21)
NONE32 = -0x80000000

VEH_IGNITION, VEH_RUNNING, VEH_HEADLIGHTS, VEH_REVERSE, VEH_MOVING, VEH_SENSORS_LIVE, VEH_NIGHT = (1 << i for i in range(7))
PREFS_METRIC, PREFS_CELSIUS, PREFS_KPA = 1, 2, 4


def _s(b: int) -> int:
    return b - 256 if b > 127 else b


def _i(b: bytes, i: int, n: int, signed: bool = False) -> int:
    return int.from_bytes(b[i:i + n], "little", signed=signed)


def _lstr(b: bytes, i: int):
    """A length-prefixed string at i: (text, next index). None when truncated."""
    if i >= len(b):
        return None
    n = b[i]
    if i + 1 + n > len(b):
        return None
    return b[i + 1:i + 1 + n].decode("utf-8", "replace"), i + 1 + n


@dataclass
class SensorDefine:
    node: int
    sensor: int
    quantity: int
    exponent: int
    flags: int
    period_ms: int
    caution_lo: int
    caution_hi: int
    warn_lo: int
    warn_hi: int
    name: str


@dataclass
class SensorValue:
    node: int
    sensor: int
    value: int  # raw
    status: int
    exponent_hint: Optional[int] = None


@dataclass
class Vehicle:
    flags: int
    backlight: int
    speed_kmh: Optional[float]


@dataclass
class Prefs:
    units: int
    theme: int
    clock24: int


@dataclass
class TimeMsg:
    utc: int
    offset_min: int
    source: int


@dataclass
class Power:
    event: int
    seconds: int


@dataclass
class Place:
    valid: bool
    cached: bool
    speed_limit: int
    mph: bool
    country: str
    street: str
    locality: str
    region: str


@dataclass
class Weather:
    observed: int
    temp_c: Optional[float]
    feels_c: Optional[float]
    code: int
    humidity: int
    wind_ms: float
    wind_from: int
    rain_pct: int
    high_c: Optional[float]
    low_c: Optional[float]
    sunrise: int
    sunset: int


@dataclass
class Timezone:
    offset_min: int
    dst: bool
    name: str


@dataclass
class OnlineStatus:
    internet_up: bool
    place_valid: bool
    weather_valid: bool
    tz_valid: bool
    last_error: int


@dataclass
class AlertDefine:
    node: int
    alert: int
    severity: int
    text: str


@dataclass
class AlertState:
    node: int
    active: int


@dataclass
class AudioStatus:
    volume: int
    flags: int
    source: int


Msg = Union[SensorDefine, SensorValue, Vehicle, Prefs, TimeMsg, Power, Place, Weather, Timezone, OnlineStatus, AlertDefine,
            AlertState, AudioStatus]


def _t16(v: int) -> Optional[float]:
    """An unsigned 16-bit read as a signed temperature in 0.1 degrees; 0x7FFF is "unknown"."""
    s = v - 0x10000 if v > 0x7FFF else v
    return None if s == 0x7FFF else s / 10.0


def decode(base: int, src: int, p: bytes) -> Optional[Msg]:
    """One received transfer (CAN_RX) to a message, or None for what the assistant does not use or cannot parse."""
    if not p:
        return None
    op = p[0]
    try:
        if base == CH_SENSOR:
            if op == 0x01 and len(p) >= 31:
                return SensorDefine(src, p[1], p[2], _s(p[3]), p[4], _i(p, 5, 2), _i(p, 15, 4, True), _i(p, 19, 4, True),
                                    _i(p, 23, 4, True), _i(p, 27, 4, True), p[31:].decode("utf-8", "replace"))
            if op == 0x02 and len(p) >= 7:
                return SensorValue(src, p[1], _i(p, 2, 4, True), p[6])
        elif base == CH_SYSTEM:
            if op == 0x02 and len(p) >= 6:
                sp = _i(p, 4, 2)
                return Vehicle(_i(p, 1, 2), p[3], None if sp == 0xFFFF else sp / 10.0)
            if op == 0x03 and len(p) >= 4:
                return Prefs(p[1], p[2], p[3])
            if op == 0x01 and len(p) >= 7:
                return TimeMsg(_i(p, 1, 4), _s(p[5]) * 15, p[6])
            if op == 0x04 and len(p) >= 3:
                return Power(p[1], p[2])
        elif base == CH_ONLINE:
            if op == 0x01 and len(p) >= 7:
                flags = p[2]
                i = 6
                parts = []
                for _ in range(3):
                    r = _lstr(p, i)
                    if r is None:
                        return None
                    parts.append(r[0])
                    i = r[1]
                return Place(bool(flags & 1), bool(flags & 2), p[3], bool(flags & 4), p[4:6].decode("ascii", "replace"),
                             parts[0], parts[1], parts[2])
            if op == 0x02 and len(p) >= 28:
                return Weather(_i(p, 1, 4), _t16(_i(p, 5, 2)), _t16(_i(p, 7, 2)), p[9], p[10], _i(p, 11, 2) / 10.0,
                               _i(p, 13, 2), p[15], _t16(_i(p, 16, 2)), _t16(_i(p, 18, 2)), _i(p, 20, 4), _i(p, 24, 4))
            if op == 0x03 and len(p) >= 4:
                r = _lstr(p, 3)
                return Timezone(_s(p[1]) * 15, bool(p[2] & 1), r[0] if r else "")
            if op == 0x05 and len(p) >= 6:
                return OnlineStatus(bool(p[1] & 1), bool(p[1] & 2), bool(p[1] & 4), bool(p[1] & 8), p[5])
        elif base == CH_ALERT:
            if op == 0x01 and len(p) >= 7:
                return AlertDefine(src, p[1], p[2], p[7:].decode("utf-8", "replace"))
            if op == 0x02 and len(p) >= 5:
                return AlertState(src, _i(p, 1, 4))
        elif base == CH_AUDIO_CMD:
            if op == 0x01 and len(p) >= 7:
                return AudioStatus(p[1], p[2], p[6])
    except (IndexError, ValueError):
        return None
    return None


# ---- what the assistant may send (the card's policy allows these, docs/hardware/ai_card.md) ----
VOLUME_SET, VOLUME_STEP, VOLUME_MUTE = 0, 1, 2
MEDIA_TOGGLE, MEDIA_PLAY, MEDIA_PAUSE, MEDIA_NEXT, MEDIA_PREV = 1, 2, 3, 4, 5
MEDIA_TUNE_UP, MEDIA_TUNE_DOWN, MEDIA_PRESET_NEXT, MEDIA_PRESET_PREV = 6, 7, 8, 9
SRC_NEXT, SRC_BLUETOOTH, SRC_RADIO, SRC_AUX, SRC_OFF = 0, 1, 2, 3, 0xFF


def audio_volume(dest: int, action: int, value: int) -> bytes:
    return bytes([0x83, dest, action, value & 0xFF])


def audio_media(dest: int, action: int, value: int = 0) -> bytes:
    return bytes([0x84, dest, action, value & 0xFF])


def audio_source(dest: int, source: int) -> bytes:
    return bytes([0x87, dest, source])


def audio_play(dest: int, sound: int, level: int = 100, repeat: int = 1, priority: int = 1) -> bytes:
    """A built-in sound on the sink (sounds 1-10), accessory priority."""
    return bytes([0x81, dest]) + sound.to_bytes(2, "little") + bytes([0x00 | (priority & 0x0F), level & 0xFF, repeat & 0xFF])
