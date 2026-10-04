"""Deterministic intents: what needs no model. Volume, media and source commands, and questions about the car's own
readings, answered from live data. Instant, always works, never leaves the card. Pure logic."""
from __future__ import annotations

import re
from dataclasses import dataclass, field
from typing import List, Optional, Tuple

from . import canmsg as C
from .context import VehicleContext, CORE_SENSORS


@dataclass
class Reply:
    text: str = ""
    actions: List[Tuple[int, bytes]] = field(default_factory=list)  # (channel base, transfer payload) for CAN_TX
    forget: bool = False


def normalise(s: str) -> str:
    s = s.lower().replace("’", "'")
    s = re.sub(r"[^a-z0-9' ]+", " ", s)
    s = re.sub(r"\b(please|hey|okay|ok|could you|can you|would you|will you)\b", " ", s)
    return re.sub(r"\s+", " ", s).strip()


def _has(t: str, pattern: str) -> bool:
    return re.search(pattern, t) is not None


def _num(t: str) -> Optional[int]:
    m = re.search(r"\b(\d{1,3})\b", t)
    return int(m.group(1)) if m else None


def handle(raw: str, ctx: VehicleContext, sink: int = 0) -> Optional[Reply]:
    """A Reply when the phrase is an intent, None when it is not (it goes to a model)."""
    t = normalise(raw)
    if not t:
        return None

    # ---- the assistant itself ----
    if _has(t, r"\b(forget (this|our|the) (conversation|chat)|start over|new conversation|reset)\b"):
        return Reply("Okay, starting fresh.", forget=True)
    if _has(t, r"^(what can you do|help|what do you do)$"):
        return Reply("I can read your gauges, run the music, and answer questions. Try asking about the oil pressure, or say next song.")

    # ---- music and volume ----
    n = _num(t)
    if _has(t, r"\bvolume\b.*\bto\b|\bset (the )?volume\b") and n is not None and n <= 100:
        return Reply(f"Volume {n}.", [(C.CH_AUDIO_CMD, C.audio_volume(sink, C.VOLUME_SET, n))])
    if _has(t, r"\bunmute\b"):
        if ctx.audio and not ctx.audio.flags & 1:
            return Reply("The sound is already on.")
        return Reply("Unmuted.", [(C.CH_AUDIO_CMD, C.audio_volume(sink, C.VOLUME_MUTE, 0))])
    if _has(t, r"\b(mute|silence|be quiet|shut up)\b") and not _has(t, r"\bmic"):
        if ctx.audio and ctx.audio.flags & 1:
            return Reply("Already muted.")
        return Reply("Muted.", [(C.CH_AUDIO_CMD, C.audio_volume(sink, C.VOLUME_MUTE, 0))])
    if _has(t, r"\b(louder|volume up|turn (it|the (volume|music|radio)) up|turn up|raise the volume|crank it)\b"):
        return Reply("Louder.", [(C.CH_AUDIO_CMD, C.audio_volume(sink, C.VOLUME_STEP, 5))])
    if _has(t, r"\b(quieter|softer|volume down|turn (it|the (volume|music|radio)) down|turn down|lower the volume)\b"):
        return Reply("Quieter.", [(C.CH_AUDIO_CMD, C.audio_volume(sink, C.VOLUME_STEP, -5))])
    if _has(t, r"\b(next (song|track|one)|skip( this| the)?( song| track| one)?)\b|^next$"):
        return Reply("Next.", [(C.CH_AUDIO_CMD, C.audio_media(sink, C.MEDIA_NEXT))])
    if _has(t, r"\b(previous (song|track|one)|last (song|track)|go back (a|one) (song|track)|play (that|the last) (again|song))\b|^previous$"):
        return Reply("Previous.", [(C.CH_AUDIO_CMD, C.audio_media(sink, C.MEDIA_PREV))])
    if _has(t, r"\b(pause|stop (the )?(music|song|playing))\b"):
        return Reply("Paused.", [(C.CH_AUDIO_CMD, C.audio_media(sink, C.MEDIA_PAUSE))])
    if _has(t, r"\b(resume|play (the )?music|keep playing|continue)\b"):
        return Reply("Playing.", [(C.CH_AUDIO_CMD, C.audio_media(sink, C.MEDIA_PLAY))])
    if _has(t, r"\b(radio|am fm)\b") and _has(t, r"\b(switch|change|go|play|put|turn)\b") and not _has(t, r"\boff\b"):
        return Reply("Radio.", [(C.CH_AUDIO_CMD, C.audio_source(sink, C.SRC_RADIO))])
    if _has(t, r"\b(bluetooth|my phone)\b") and _has(t, r"\b(switch|change|go|play|use)\b"):
        return Reply("Bluetooth.", [(C.CH_AUDIO_CMD, C.audio_source(sink, C.SRC_BLUETOOTH))])
    if _has(t, r"\b(turn|switch) (off|the audio off|the music off|the radio off)\b") and _has(t, r"\b(music|audio|radio|sound)\b"):
        return Reply("Audio off.", [(C.CH_AUDIO_CMD, C.audio_source(sink, C.SRC_OFF))])

    # ---- the car's own readings ----
    sensor = _sensor_topic(t)
    if sensor is not None:
        return _read(ctx, sensor)
    if _has(t, r"\b(any|are there)\b.*\b(alerts?|warnings?|problems?|faults?)\b|\banything wrong\b|how'?s the car|everything (ok|fine|alright)"):
        issues = ctx.active_alerts() + ctx.faulty_sensors()
        if issues:
            return Reply("Heads up: " + "; ".join(issues[:3]) + ".")
        return Reply("No alerts. Everything I can see looks normal.")

    # ---- time and place (from the car's clock and the gateway) ----
    if _has(t, r"\bwhat('s| is) the time\b|\bwhat time is it\b|\bcurrent time\b|\bthe time\b"):
        s = ctx.spoken_time()
        return Reply(f"It's {s}." if s else "I don't have the time yet.")
    if _has(t, r"\bwhere am i\b|\bwhat (street|road|town|city) (am i|is this)\b|\bwhat('s| is) (this|the) (street|road|town|city)\b"):
        if ctx.place and ctx.place[0].valid:
            p = ctx.place[0]
            return Reply("You're " + (f"on {p.street}, " if p.street else "") + (f"in {p.locality}" if p.locality else "near here") + ".")
        return Reply("I don't have a location yet.")
    if _has(t, r"\b(weather|forecast|will it rain|rain today|how hot (is it|will it be) today)\b"):
        if ctx.weather and ctx.weather[0].temp_c is not None:
            w = ctx.weather[0]
            def tmp(c):
                return f"{c:.0f} degrees Celsius" if ctx.celsius else f"{c * 9 / 5 + 32:.0f} degrees Fahrenheit"
            s = f"It's {tmp(w.temp_c)} outside"
            if w.high_c is not None:
                s += f", with a high of {tmp(w.high_c)}"
            if w.rain_pct != 0xFF and w.rain_pct >= 30:
                s += f", and a {w.rain_pct} percent chance of rain"
            return Reply(s + ".")
        return None  # a model (online) may know
    return None


_TOPICS = [
    # (name, pattern, core sensor number); order matters: more specific first
    ("ambient", r"\b(outside|ambient|exterior)\b.*\b(temp|temperature|cold|warm|hot)\b|\bhow (cold|warm|hot) is it outside\b", 6),
    ("oil", r"\boil\b", 4),
    ("engine temp", r"\b(engine|coolant|water|motor)\b.*\b(temp|temperature|hot)\b|\b(overheat|running hot|how hot is (it|the engine))\b|\bengine temp", 3),
    ("fuel", r"\b(how much )?(fuel|gas|gasoline)( is)?( left| do i have| in the tank)?\b.*|\bfuel level\b|\btank\b", 2),
    ("mpg", r"\bmpg\b|\bfuel economy\b|\bgas mileage\b|\bmiles per gallon\b", 9),
    ("range", r"\bhow far\b.*\b(go|drive|get)\b|\b(range|distance to empty)\b|\bmiles (left|to empty)\b", 8),
    ("odometer", r"\bodometer\b|\bhow many miles (are )?on\b|\bmileage\b", 7),
    ("rpm", r"\b(rpm|revs|tach|engine speed)\b|\bhow fast (is|are) (the )?engine\b", 1),
    ("speed", r"\b(how fast|speed|mph)\b", 0),
    ("battery", r"\b(battery|voltage|charging|alternator)\b", 5),
]


def _sensor_topic(t: str) -> Optional[int]:
    for _, pat, sensor in _TOPICS:
        if re.search(pat, t):
            return sensor
    return None


def _read(ctx: VehicleContext, sensor: int) -> Reply:
    r = ctx.core(sensor)
    name = CORE_SENSORS[sensor][0]
    if r is None or r.stale:
        return Reply(f"I'm not getting the {name} right now.")
    if r.value is None:
        from .context import STATUS_TEXT
        return Reply(f"The {name} sensor is {STATUS_TEXT.get(r.status, 'unavailable')}.")
    s = ctx.speak(r)
    if sensor == 5:
        return Reply("Yes, the alternator is charging." if r.value else "No, the alternator is not charging.")
    if sensor == 2:
        return Reply(f"The tank is at {s}.")
    if sensor == 8:
        return Reply(f"About {s} to empty.")
    if sensor in (9, 10):
        return Reply(f"You're getting {s}.")
    return Reply(f"The {name} is {s}.")
