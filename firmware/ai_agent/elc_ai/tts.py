"""Text to speech, on the card (piper). Returns raw 16-bit mono PCM and its sample rate."""
from __future__ import annotations

import math
import re
import struct
import subprocess
from typing import Iterator, List, Tuple

from .link import SPEECH_RATES


def clean_for_speech(text: str) -> str:
    """Markdown and symbols out; a voice reads them aloud otherwise."""
    t = re.sub(r"```.*?```", " ", text, flags=re.S)
    t = re.sub(r"[*_`#>|]+", " ", t)
    t = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", t)
    t = re.sub(r"https?://\S+", " ", t)
    t = t.replace("°F", " degrees Fahrenheit").replace("°C", " degrees Celsius").replace("°", " degrees")
    t = t.replace("&", " and ").replace("%", " percent")
    return re.sub(r"\s+", " ", t).strip()


def split_sentences(buf: str) -> Tuple[List[str], str]:
    """Complete sentences in buf, and the unfinished rest. A sentence ends at . ! ? followed by a space, or a newline."""
    out: List[str] = []
    start = 0
    for m in re.finditer(r"([.!?]+)(\s+)|\n+", buf):
        s = buf[start:m.start()] + (m.group(1) or "")
        # a decimal point or an abbreviation is not the end
        if m.group(1) and re.search(r"\b(Mr|Mrs|Dr|St|No|vs|etc)$", buf[start:m.start()]):
            continue
        if s.strip():
            out.append(s.strip())
        start = m.end()
    return out, buf[start:]


class TtsBackend:
    name = "tts"
    rate = 22050

    def synth(self, text: str) -> bytes:  # pragma: no cover - interface
        raise NotImplementedError

    @property
    def rate_code(self) -> int:
        return SPEECH_RATES.index(self.rate)


class PiperTts(TtsBackend):
    name = "piper"

    def __init__(self, model: str, rate: int = 22050, binary: str = "piper", extra: Tuple[str, ...] = ()):
        if rate not in SPEECH_RATES:
            raise ValueError(f"rate {rate} is not one the link carries: {SPEECH_RATES}")
        self.model, self.rate, self.binary, self.extra = model, rate, binary, extra

    def synth(self, text: str) -> bytes:
        p = subprocess.run([self.binary, "--model", self.model, "--output_raw", *self.extra],
                           input=text.encode(), stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=60, check=True)
        return p.stdout


class FakeTts(TtsBackend):
    """A tone per word, for tests and for a bench with no voice model."""
    name = "fake"

    def __init__(self, rate: int = 16000):
        self.rate = rate
        self.spoken: List[str] = []

    def synth(self, text: str) -> bytes:
        self.spoken.append(text)
        words = max(1, len(text.split()))
        n = int(self.rate * 0.15 * words)
        return b"".join(struct.pack("<h", int(6000 * math.sin(2 * math.pi * 440 * i / self.rate))) for i in range(n))
