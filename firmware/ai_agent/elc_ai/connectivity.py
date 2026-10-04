"""Is an online lookup possible? A cheap probe on a timer, with hysteresis, so one lost packet does not flip the answer."""
from __future__ import annotations

import time
from typing import Callable, Optional


class Connectivity:
    def __init__(self, probe: Callable[[], bool], interval_s: float = 15.0, down_after: int = 2, up_after: int = 1,
                 clock: Callable[[], float] = time.monotonic):
        self._probe = probe
        self._interval = interval_s
        self._down_after = down_after
        self._up_after = up_after
        self._clock = clock
        self._up = False
        self._bad = 0
        self._good = 0
        self._next = 0.0
        self.last_change: Optional[float] = None

    @property
    def up(self) -> bool:
        return self._up

    def hint(self, gateway_internet_up: bool) -> None:
        """The LTE gateway says it has internet (ONLINE STATUS bit0): worth a probe now, never enough to say 'up' alone."""
        if gateway_internet_up and not self._up:
            self._next = 0.0

    def mark_failure(self) -> None:
        """A real request failed: count it as a bad probe and look again soon."""
        self._record(False)
        self._next = self._clock() + 3.0

    def check(self, force: bool = False) -> bool:
        now = self._clock()
        if force or now >= self._next:
            self._next = now + self._interval
            self._record(bool(self._safe_probe()))
        return self._up

    def _safe_probe(self) -> bool:
        try:
            return self._probe()
        except Exception:
            return False

    def _record(self, ok: bool) -> None:
        if ok:
            self._bad = 0
            self._good += 1
            if not self._up and self._good >= self._up_after:
                self._up, self.last_change = True, self._clock()
        else:
            self._good = 0
            self._bad += 1
            if self._up and self._bad >= self._down_after:
                self._up, self.last_change = False, self._clock()
