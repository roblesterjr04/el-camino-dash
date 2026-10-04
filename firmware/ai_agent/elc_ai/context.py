"""What the assistant knows about the car: the latest SENSOR / SYSTEM / ONLINE / ALERT messages from the card's CAN_RX,
with units conversion for speech. Pure logic, no I/O."""
from __future__ import annotations

import time
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple

from . import canmsg as C

# The center's core sensors (can_channels.md, "Core sensors (node 0x01)"): known before their DEFINE arrives.
CORE_SENSORS = {
    0: ("road speed", C.Q_SPEED, -1, 1000), 1: ("engine speed", C.Q_RPM, 0, 1000), 2: ("fuel level", C.Q_PERCENT, -1, 1000),
    3: ("engine temperature", C.Q_TEMP, -1, 1000), 4: ("oil pressure", C.Q_PRESSURE, 0, 1000), 5: ("charging", C.Q_SWITCH, 0, 1000),
    6: ("ambient temperature", C.Q_TEMP, -1, 3000), 7: ("odometer", C.Q_DISTANCE, 0, 3000),
    8: ("distance to empty", C.Q_DISTANCE, 0, 3000), 9: ("fuel economy", C.Q_ECONOMY, -2, 3000),
    10: ("trip fuel economy", C.Q_ECONOMY, -2, 3000),
}
CENTER = 0x01
STATUS_TEXT = {1: "not ready", 2: "open circuit", 3: "short circuit", 4: "out of range", 5: "faulted"}


@dataclass
class Reading:
    node: int
    sensor: int
    name: str
    quantity: int
    value: Optional[float]  # canonical unit, None when the status is not ok
    status: int
    age_s: float
    stale: bool


class VehicleContext:
    def __init__(self, clock=time.monotonic):
        self._clock = clock
        self.defs: Dict[Tuple[int, int], C.SensorDefine] = {}
        self.values: Dict[Tuple[int, int], Tuple[C.SensorValue, float]] = {}
        self.vehicle: Optional[C.Vehicle] = None
        self.prefs = C.Prefs(0, 0, 0)  # default: miles, Fahrenheit, psi
        self.have_prefs = False
        self.time_msg: Optional[Tuple[C.TimeMsg, float]] = None
        self.place: Optional[Tuple[C.Place, float]] = None
        self.weather: Optional[Tuple[C.Weather, float]] = None
        self.tz: Optional[C.Timezone] = None
        self.online: Optional[Tuple[C.OnlineStatus, float]] = None
        self.alert_defs: Dict[Tuple[int, int], C.AlertDefine] = {}
        self.alert_active: Dict[int, int] = {}
        self.audio: Optional[C.AudioStatus] = None
        self.nodes: Dict[int, Tuple[int, int]] = {}  # node -> (type, product)

    # ---- input ----
    def ingest(self, base: int, src: int, payload: bytes) -> None:
        m = C.decode(base, src, payload)
        now = self._clock()
        if isinstance(m, C.SensorDefine):
            self.defs[(m.node, m.sensor)] = m
        elif isinstance(m, C.SensorValue):
            self.values[(m.node, m.sensor)] = (m, now)
        elif isinstance(m, C.Vehicle):
            self.vehicle = m
        elif isinstance(m, C.Prefs):
            self.prefs, self.have_prefs = m, True
        elif isinstance(m, C.TimeMsg):
            self.time_msg = (m, now)
        elif isinstance(m, C.Place):
            self.place = (m, now)
        elif isinstance(m, C.Weather):
            self.weather = (m, now)
        elif isinstance(m, C.Timezone):
            self.tz = m
        elif isinstance(m, C.OnlineStatus):
            self.online = (m, now)
        elif isinstance(m, C.AlertDefine):
            self.alert_defs[(m.node, m.alert)] = m
        elif isinstance(m, C.AlertState):
            self.alert_active[m.node] = m.active
        elif isinstance(m, C.AudioStatus):
            self.audio = m

    def set_nodes(self, nodes: Dict[int, Tuple[int, int]]) -> None:
        self.nodes = dict(nodes)

    # ---- sensors ----
    def _reading(self, key: Tuple[int, int]) -> Optional[Reading]:
        got = self.values.get(key)
        if not got:
            return None
        v, t = got
        d = self.defs.get(key)
        core = CORE_SENSORS.get(key[1]) if key[0] == CENTER else None
        if d:
            name, q, exp, period = d.name, d.quantity, d.exponent, d.period_ms
        elif core:
            name, q, exp, period = core
        else:
            name, q, exp, period = f"sensor {key[1]}", C.Q_UNITLESS, 0, 1000
        age = self._clock() - t
        stale = age > max(3 * period / 1000.0, 0.5)
        val = None if v.status != 0 or v.value == C.NONE32 else v.value * (10.0 ** exp)
        return Reading(key[0], key[1], name, q, val, v.status, age, stale)

    def core(self, sensor: int) -> Optional[Reading]:
        return self._reading((CENTER, sensor))

    def all_readings(self) -> List[Reading]:
        return [r for k in sorted(self.values) if (r := self._reading(k))]

    def find(self, words: str) -> Optional[Reading]:
        """An accessory's reading by (part of) its name."""
        w = words.lower()
        for r in self.all_readings():
            if w in r.name.lower():
                return r
        return None

    # ---- units and speech ----
    @property
    def metric(self) -> bool:
        return bool(self.prefs.units & C.PREFS_METRIC)

    @property
    def celsius(self) -> bool:
        return bool(self.prefs.units & C.PREFS_CELSIUS)

    @property
    def kpa(self) -> bool:
        return bool(self.prefs.units & C.PREFS_KPA)

    def speak(self, r: Reading) -> str:
        """A reading as it is said: '42 psi', '195 degrees Fahrenheit', '55 miles per hour'."""
        if r.value is None:
            return STATUS_TEXT.get(r.status, "unavailable")
        v, q = r.value, r.quantity
        if q == C.Q_SPEED:
            return f"{v:.0f} kilometers per hour" if self.metric else f"{v * 0.621371:.0f} miles per hour"
        if q == C.Q_TEMP:
            return f"{v:.0f} degrees Celsius" if self.celsius else f"{v * 9 / 5 + 32:.0f} degrees Fahrenheit"
        if q == C.Q_PRESSURE:
            return f"{v:.0f} kilopascals" if self.kpa else f"{v * 0.145038:.0f} psi"
        if q == C.Q_DISTANCE:
            return f"{v / 1000:.0f} kilometers" if self.metric else f"{v / 1609.344:.0f} miles"
        if q == C.Q_ECONOMY:
            return f"{100 / v:.1f} liters per 100 kilometers" if self.metric and v > 0 else f"{v * 2.35215:.1f} miles per gallon"
        if q == C.Q_RPM:
            return f"{v:.0f} rpm"
        if q == C.Q_PERCENT:
            return f"{v:.0f} percent"
        if q == C.Q_VOLTAGE:
            return f"{v:.1f} volts"
        if q == C.Q_SWITCH:
            return "yes" if v else "no"
        return f"{v:g}"

    # ---- time and place ----
    def local_time(self) -> Optional[time.struct_time]:
        if not self.time_msg:
            return None
        m, t = self.time_msg
        utc = m.utc + (self._clock() - t) + m.offset_min * 60
        return time.gmtime(int(utc))

    def spoken_time(self) -> Optional[str]:
        lt = self.local_time()
        if not lt:
            return None
        if self.prefs.clock24:
            return f"{lt.tm_hour:02d}:{lt.tm_min:02d}"
        h = lt.tm_hour % 12 or 12
        return f"{h}:{lt.tm_min:02d} {'AM' if lt.tm_hour < 12 else 'PM'}"

    def town(self) -> Optional[str]:
        if not self.place:
            return None
        p = self.place[0]
        if not p.valid:
            return None
        return ", ".join(x for x in (p.locality, p.region) if x) or None

    def active_alerts(self) -> List[str]:
        out = []
        for node, mask in self.alert_active.items():
            for n in range(32):
                if mask & (1 << n):
                    d = self.alert_defs.get((node, n))
                    out.append(d.text if d else f"alert {n} from node {node}")
        return out

    def faulty_sensors(self) -> List[str]:
        out = []
        for r in self.all_readings():
            if r.status != 0 and r.status != 1:
                out.append(f"{r.name}: {STATUS_TEXT.get(r.status, 'fault')}")
        return out

    # ---- for the local model's prompt ----
    def snapshot(self) -> str:
        """The live state as a few lines of text, in the driver's units."""
        lines = []
        if self.vehicle:
            v = self.vehicle
            lines.append("Ignition on" if v.flags & C.VEH_IGNITION else "Ignition off")
            if v.flags & C.VEH_RUNNING:
                lines.append("Engine running")
        for r in self.all_readings():
            if r.stale:
                continue
            lines.append(f"{r.name}: {self.speak(r)}")
        t = self.spoken_time()
        if t:
            lines.append(f"Local time {t}")
        town = self.town()
        if town:
            lines.append(f"Location: {town}")
        if self.weather and self.weather[0].temp_c is not None:
            w = self.weather[0]
            tc = w.temp_c
            lines.append("Outside (online report): " + (f"{tc:.0f} C" if self.celsius else f"{tc * 9 / 5 + 32:.0f} F"))
        al = self.active_alerts()
        if al:
            lines.append("Active alerts: " + "; ".join(al))
        return "\n".join(lines) if lines else "No live data yet."

    def online_context(self) -> str:
        """The only vehicle context sent with an online question: time, town, units. Never sensors, never a position."""
        parts = []
        t = self.spoken_time()
        if t:
            parts.append(f"The local time is {t}.")
        town = self.town()
        if town:
            parts.append(f"The driver is near {town}.")
        parts.append("Use metric units." if self.metric else "Use US units (miles, Fahrenheit).")
        return " ".join(parts)
