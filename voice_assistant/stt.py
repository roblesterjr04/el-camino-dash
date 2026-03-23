"""Speech-to-text using faster-whisper."""

from __future__ import annotations

import logging

import numpy as np
from faster_whisper import WhisperModel

from config import WHISPER_COMPUTE_TYPE, WHISPER_DEVICE, WHISPER_MODEL

logger = logging.getLogger(__name__)

_model = WhisperModel(
    WHISPER_MODEL,
    device=WHISPER_DEVICE,
    compute_type=WHISPER_COMPUTE_TYPE,
)
logger.info("Whisper model loaded: %s", WHISPER_MODEL)


def transcribe(audio: np.ndarray) -> str:
    """Transcribe mono float32 16 kHz audio to text."""
    try:
        segments, _info = _model.transcribe(
            audio,
            language="en",
            beam_size=5,
        )
        parts: list[str] = []
        for seg in segments:
            parts.append(seg.text)
        text = "".join(parts).strip()
        logger.info('[stt] "%s"', text)
        return text
    except Exception as exc:
        logger.warning("STT failed: %s", exc)
        return ""
