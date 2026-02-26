from __future__ import annotations

from collections import defaultdict
from dataclasses import dataclass
from typing import Any, Callable


@dataclass
class DashboardEvent:
    topic: str
    payload: dict[str, Any]


class EventBus:
    def __init__(self) -> None:
        self._handlers: dict[str, list[Callable[[DashboardEvent], None]]] = defaultdict(list)

    def subscribe(self, topic: str, handler: Callable[[DashboardEvent], None]) -> None:
        self._handlers[topic].append(handler)

    def publish(self, topic: str, payload: dict[str, Any]) -> None:
        event = DashboardEvent(topic=topic, payload=payload)
        for handler in self._handlers.get(topic, []):
            handler(event)
