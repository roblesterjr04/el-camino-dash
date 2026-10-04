"""Where a question that is not an intent goes: the on-board model or the online one."""
from __future__ import annotations

import re

LOCAL, ONLINE = "local", "online"
AUTO, ALWAYS, OFFLINE_ONLY = 0, 1, 2  # the menu's "Online lookups"

_FRESH = re.compile(
    r"\b(news|headline|today|tonight|tomorrow|yesterday|this (week|weekend|morning)|latest|current|currently|right now|"
    r"near me|nearby|closest|nearest|open now|opening hours|hours of|price of|how much (does|is|are)|cost of|score|"
    r"who (won|is the (president|ceo))|stock|traffic|directions|route to|how do i get to|search|look up|google|"
    r"what year did|when did|who (is|was|are)|what is the (population|capital)|recall|release date|review)\b")
# A question about the car's own state or this car: stays on the card (grounding, and it is the driver's own data).
_CAR = re.compile(r"\b(my (car|truck|el camino|engine|gauge|oil|tank|battery)|the (car|engine|gauge|tank|alternator)|"
                  r"this (car|truck))\b")


def needs_fresh(text: str) -> bool:
    return bool(_FRESH.search(text.lower()))


def about_the_car(text: str) -> bool:
    return bool(_CAR.search(text.lower()))


def route(text: str, mode: int, online_up: bool) -> str:
    """LOCAL or ONLINE for a free-form question."""
    if mode == OFFLINE_ONLY or not online_up:
        return LOCAL
    if about_the_car(text) and not needs_fresh(text):
        return LOCAL
    if mode == ALWAYS:
        return ONLINE
    return ONLINE if needs_fresh(text) else LOCAL
