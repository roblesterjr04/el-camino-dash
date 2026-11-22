"""Speech to text, on the card. Audio never leaves the car."""
from __future__ import annotations

import io
import json
import urllib.request
import uuid
import wave
from typing import Callable, Optional


def wav_bytes(pcm16k: bytes, rate: int = 16000) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm16k)
    return buf.getvalue()


class SttBackend:
    name = "stt"

    def transcribe(self, pcm16k: bytes) -> str:  # pragma: no cover - interface
        raise NotImplementedError


class WhisperServerStt(SttBackend):
    """whisper.cpp's `whisper-server` on localhost (POST /inference, multipart WAV)."""
    name = "whisper.cpp"

    def __init__(self, url: str = "http://127.0.0.1:8081", timeout_s: float = 20.0, prompt: str = ""):
        self.url, self.timeout, self.prompt = url.rstrip("/"), timeout_s, prompt

    def transcribe(self, pcm16k: bytes) -> str:
        boundary = uuid.uuid4().hex
        fields = {"response_format": "json", "temperature": "0.0"}
        if self.prompt:
            fields["prompt"] = self.prompt  # words to expect (the car's vocabulary)
        body = b""
        for k, v in fields.items():
            body += f'--{boundary}\r\nContent-Disposition: form-data; name="{k}"\r\n\r\n{v}\r\n'.encode()
        body += (f'--{boundary}\r\nContent-Disposition: form-data; name="file"; filename="a.wav"\r\n'
                 "Content-Type: audio/wav\r\n\r\n").encode() + wav_bytes(pcm16k) + f"\r\n--{boundary}--\r\n".encode()
        req = urllib.request.Request(self.url + "/inference", data=body,
                                     headers={"Content-Type": f"multipart/form-data; boundary={boundary}"})
        with urllib.request.urlopen(req, timeout=self.timeout) as r:
            return clean_transcript(json.loads(r.read().decode()).get("text", ""))


def clean_transcript(s: str) -> str:
    """Whisper writes [BLANK_AUDIO], (music) and the like for non-speech: that is no phrase."""
    import re
    s = re.sub(r"\[[^\]]*\]|\([^)]*\)|\*[^*]*\*", " ", s)
    return re.sub(r"\s+", " ", s).strip()


class FakeStt(SttBackend):
    name = "fake"

    def __init__(self, result: str = "", fn: Optional[Callable[[bytes], str]] = None):
        self.result, self.fn, self.calls = result, fn, []

    def transcribe(self, pcm16k: bytes) -> str:
        self.calls.append(len(pcm16k))
        return self.fn(pcm16k) if self.fn else self.result
