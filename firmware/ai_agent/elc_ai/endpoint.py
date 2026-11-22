"""End-of-speech detection on the 16 kHz microphone stream from the card: an energy gate with an adaptive noise floor.
The card forwards audio from the wake word on; this decides when the phrase is over."""
from __future__ import annotations

import array
import math
from typing import Optional


class Endpointer:
    FRAME = 320  # 20 ms at 16 kHz, the card's AUDIO frame

    def __init__(self, silence_ms: int = 800, max_ms: int = 10000, no_speech_ms: int = 4000, min_speech_ms: int = 160,
                 floor_rms: float = 150.0):
        self.silence_ms = silence_ms
        self.max_ms = max_ms
        self.no_speech_ms = no_speech_ms
        self.min_speech_ms = min_speech_ms
        self.floor = floor_rms
        self.reset()

    def reset(self) -> None:
        self.elapsed_ms = 0
        self.speech_ms = 0
        self.quiet_ms = 0
        self.started = False
        self.done = False
        self.reason: Optional[str] = None
        self._noise = self.floor
        self._calibrating_ms = 0

    @staticmethod
    def rms(pcm: bytes) -> float:
        a = array.array("h")
        a.frombytes(pcm[: len(pcm) // 2 * 2])
        if not a:
            return 0.0
        return math.sqrt(sum(x * x for x in a) / len(a))

    def feed(self, pcm: bytes) -> bool:
        """Feed one frame (any length). True once the phrase is over (see .reason: 'silence', 'max', 'nospeech')."""
        if self.done:
            return True
        ms = max(1, len(pcm) // 2 * 1000 // 16000)
        self.elapsed_ms += ms
        level = self.rms(pcm)
        threshold = max(self._noise * 3.0, 450.0)
        if not self.started:
            # the first 200 ms set the noise floor (the wake word's own tail is excluded by the card)
            if self._calibrating_ms < 200:
                self._calibrating_ms += ms
                self._noise = max(self.floor, 0.7 * self._noise + 0.3 * level)
            if level > threshold:
                self.speech_ms += ms
                if self.speech_ms >= self.min_speech_ms:
                    self.started = True
                    self.quiet_ms = 0
            else:
                self.speech_ms = 0
                self._noise = max(self.floor, 0.95 * self._noise + 0.05 * level)
            if not self.started and self.elapsed_ms >= self.no_speech_ms:
                self.done, self.reason = True, "nospeech"
        else:
            if level > threshold * 0.6:
                self.quiet_ms = 0
            else:
                self.quiet_ms += ms
                if self.quiet_ms >= self.silence_ms:
                    self.done, self.reason = True, "silence"
            if self.elapsed_ms >= self.max_ms:
                self.done, self.reason = True, self.reason or "max"
        return self.done
