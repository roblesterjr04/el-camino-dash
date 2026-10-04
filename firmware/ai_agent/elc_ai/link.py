"""The AI link (docs/protocols/ai_link.md): COBS-framed, CRC-16 protected messages. Mirrors firmware/ai_card/src/ai_link.*."""
from __future__ import annotations

from dataclasses import dataclass
from typing import Iterator, List, Optional

# ESP32 -> module
T_HELLO, T_PING, T_SHUTDOWN, T_SETTINGS = 0x01, 0x02, 0x03, 0x04
T_LISTEN, T_AUDIO, T_CANCEL, T_SPOKEN = 0x10, 0x11, 0x12, 0x13
T_CAN_RX, T_CAN_NODES, T_CTX = 0x20, 0x21, 0x22
T_CAN_TX_RESULT, T_SPEECH_CREDIT = 0x30, 0x31
# module -> ESP32
T_READY, T_PONG, T_SHUTDOWN_ACK = 0x81, 0x82, 0x83
T_STATE, T_LISTEN_STOP, T_TEXT = 0x90, 0x91, 0x92
T_SPEECH, T_SPEECH_END, T_SPEECH_CANCEL = 0xA0, 0xA1, 0xA2
T_CAN_TX, T_SUBSCRIBE, T_CTX_REQUEST, T_LOG = 0xB0, 0xB1, 0xB2, 0xF0

MAX_PAYLOAD = 1024
SPEECH_RATES = [16000, 22050, 24000, 44100, 48000]  # SPEECH's rate code


def crc16(data: bytes) -> int:
    """CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF): the same as the CAN channels."""
    c = 0xFFFF
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def cobs_encode(data: bytes) -> bytes:
    out = bytearray([0])
    code_at, code = 0, 1
    for b in data:
        if b == 0:
            out[code_at] = code
            code_at = len(out)
            out.append(0)
            code = 1
        else:
            out.append(b)
            code += 1
            if code == 0xFF:
                out[code_at] = code
                code_at = len(out)
                out.append(0)
                code = 1
    out[code_at] = code
    return bytes(out)


def cobs_decode(data: bytes) -> Optional[bytes]:
    if not data:
        return None
    out = bytearray()
    i = 0
    n = len(data)
    while i < n:
        code = data[i]
        i += 1
        if code == 0:
            return None
        for _ in range(code - 1):
            if i >= n or data[i] == 0:
                return None
            out.append(data[i])
            i += 1
        if code != 0xFF and i < n:
            out.append(0)
    return bytes(out)


def build_frame(ftype: int, seq: int, payload: bytes = b"") -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload too long")
    body = bytes([ftype, seq & 0xFF]) + payload
    crc = crc16(body)
    return cobs_encode(body + bytes([crc & 0xFF, crc >> 8])) + b"\x00"


@dataclass
class Frame:
    type: int
    seq: int
    payload: bytes


class Parser:
    """Feed bytes as they arrive; yields the good frames. Counts what it dropped."""

    def __init__(self) -> None:
        self._buf = bytearray()
        self._overflow = False
        self._next_seq: Optional[int] = None
        self.frames = 0
        self.bad = 0
        self.gaps = 0

    def feed(self, data: bytes) -> Iterator[Frame]:
        for b in data:
            if b != 0:
                if len(self._buf) < MAX_PAYLOAD + 64:
                    self._buf.append(b)
                else:
                    self._overflow = True
                continue
            raw, over = bytes(self._buf), self._overflow
            self._buf.clear()
            self._overflow = False
            if not raw and not over:
                continue
            if over:
                self.bad += 1
                continue
            body = cobs_decode(raw)
            if body is None or len(body) < 4:
                self.bad += 1
                continue
            if (body[-2] | body[-1] << 8) != crc16(body[:-2]):
                self.bad += 1
                continue
            if self._next_seq is not None and body[1] != self._next_seq:
                self.gaps += (body[1] - self._next_seq) & 0xFF
            self._next_seq = (body[1] + 1) & 0xFF
            self.frames += 1
            yield Frame(body[0], body[1], body[2:-2])


class Sender:
    """Owns the sequence number."""

    def __init__(self) -> None:
        self._seq = 0

    def frame(self, ftype: int, payload: bytes = b"") -> bytes:
        f = build_frame(ftype, self._seq, payload)
        self._seq = (self._seq + 1) & 0xFF
        return f


# ---- payload builders / readers (little-endian) ----
def u8(v: int) -> bytes:
    return bytes([v & 0xFF])


def u16(v: int) -> bytes:
    return (v & 0xFFFF).to_bytes(2, "little")


def u32(v: int) -> bytes:
    return (v & 0xFFFFFFFF).to_bytes(4, "little")


def read_u16(b: bytes, i: int = 0) -> int:
    return int.from_bytes(b[i:i + 2], "little")


def read_s32(b: bytes, i: int = 0) -> int:
    return int.from_bytes(b[i:i + 4], "little", signed=True)


# ---- message helpers: each returns the complete payload ----
def ready(version: int, flags: int, name: str) -> bytes:
    return u16(version) + u8(flags) + name.encode()[:24]


def pong(counter: int, soc_c: int, load: int, flags: int) -> bytes:
    return u8(counter) + (soc_c & 0xFF).to_bytes(1, "little") + u8(load) + u8(flags)


def state(st: int, source: int) -> bytes:
    return u8(st) + u8(source)


def text(kind: int, s: str, limit: int = 200) -> bytes:
    return u8(kind) + s.encode("utf-8")[:limit].decode("utf-8", "ignore").encode("utf-8")


def speech(rate_code: int, pcm: bytes) -> bytes:
    return u8(rate_code) + pcm


def can_tx(tag: int, base: int, payload: bytes) -> bytes:
    return u8(tag) + u16(base) + payload


def subscribe(bases: List[int]) -> bytes:
    return u8(len(bases)) + b"".join(u16(b) for b in bases)
