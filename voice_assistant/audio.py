"""Audio I/O: microphone via sounddevice, playback via ALSA / I2S / PulseAudio."""

from __future__ import annotations

import logging
import subprocess
import wave
from pathlib import Path

import numpy as np
import sounddevice as sd

from config import (
    ALSA_OUTPUT_DEVICE,
    AUDIO_OUTPUT_BACKEND,
    MIC_CHANNELS,
    MIC_DEVICE_INDEX,
    MIC_SAMPLE_RATE,
    OUTPUT_SAMPLE_RATE,
)

logger = logging.getLogger(__name__)


class AudioBackendError(RuntimeError):
    """Raised when the configured audio output backend is invalid or fails."""


def _load_wav_float32(path: str) -> tuple[np.ndarray, int]:
    with wave.open(path, "rb") as wf:
        ch = wf.getnchannels()
        width = wf.getsampwidth()
        rate = wf.getframerate()
        nframes = wf.getnframes()
        raw = wf.readframes(nframes)
    if width != 2:
        raise AudioBackendError(f"Unsupported WAV sample width: {width} bytes (need 16-bit)")
    audio = np.frombuffer(raw, dtype=np.int16).astype(np.float32) / 32768.0
    if ch == 2:
        audio = audio.reshape(-1, 2).mean(axis=1)
    return audio, rate


def record_audio(duration_s: float) -> np.ndarray:
    """Record mono float32 audio at MIC_SAMPLE_RATE."""
    n = int(MIC_SAMPLE_RATE * duration_s)
    audio = sd.rec(
        n,
        samplerate=MIC_SAMPLE_RATE,
        channels=MIC_CHANNELS,
        dtype="float32",
        device=MIC_DEVICE_INDEX,
    )
    sd.wait()
    if audio.ndim > 1:
        audio = audio[:, 0]
    return np.asarray(audio, dtype=np.float32)


def play_audio(wav_path: str) -> None:
    path = Path(wav_path)
    if not path.is_file():
        raise AudioBackendError(f"WAV file not found: {wav_path}")

    backend = AUDIO_OUTPUT_BACKEND.lower()
    if backend == "alsa":
        r = subprocess.run(
            ["aplay", "-D", ALSA_OUTPUT_DEVICE, str(path)],
            capture_output=True,
            text=True,
        )
        if r.returncode != 0:
            raise AudioBackendError(f"aplay failed ({r.returncode}): {r.stderr.strip()}")
    elif backend == "i2s":
        audio, sr = _load_wav_float32(str(path))
        try:
            dev = ALSA_OUTPUT_DEVICE if ALSA_OUTPUT_DEVICE != "default" else None
            if dev is not None and dev.startswith("hw:"):
                sd.play(audio, sr, device=dev)
            else:
                sd.play(audio, sr)
            sd.wait()
        except Exception as exc:
            raise AudioBackendError(f"I2S/sounddevice playback failed: {exc}") from exc
    elif backend == "pulse":
        r = subprocess.run(["paplay", str(path)], capture_output=True, text=True)
        if r.returncode != 0:
            raise AudioBackendError(f"paplay failed ({r.returncode}): {r.stderr.strip()}")
    else:
        raise AudioBackendError(
            f"Unknown AUDIO_OUTPUT_BACKEND {AUDIO_OUTPUT_BACKEND!r}; use 'alsa', 'i2s', or 'pulse'"
        )


def get_audio_stream():
    """Yield float32 mono PCM chunks from the microphone (blocking)."""
    chunk_samples = int(MIC_SAMPLE_RATE * (MIC_CHUNK_DURATION_MS / 1000.0))
    if chunk_samples < 1:
        chunk_samples = 512

    stream = sd.InputStream(
        samplerate=MIC_SAMPLE_RATE,
        channels=MIC_CHANNELS,
        dtype="float32",
        device=MIC_DEVICE_INDEX,
        blocksize=chunk_samples,
    )
    stream.start()
    try:
        while True:
            data, _overflowed = stream.read(chunk_samples)
            if data.ndim > 1:
                data = data[:, 0]
            yield np.asarray(data, dtype=np.float32).copy()
    finally:
        stream.stop()
        stream.close()


def log_active_backend() -> None:
    logger.info(
        "audio output backend=%s alsa_device=%s mic_rate=%s",
        AUDIO_OUTPUT_BACKEND,
        ALSA_OUTPUT_DEVICE,
        MIC_SAMPLE_RATE,
    )
