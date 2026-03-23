"""Wake word detection using openWakeWord."""

from __future__ import annotations

import logging

import numpy as np
from openwakeword.model import Model

import audio
from config import WAKE_WORD_MODEL, WAKE_WORD_THRESHOLD

logger = logging.getLogger(__name__)

logging.getLogger("openwakeword").setLevel(logging.WARNING)

_oww_model: Model | None = None

# openWakeWord expects 16 kHz frames; 80 ms = 1280 samples per predict() step.
_OWW_FRAME_SAMPLES = 1280


def load_model() -> None:
    """Load the wake word model once (downloads pretrained assets if needed)."""
    global _oww_model
    if _oww_model is not None:
        return
    _oww_model = Model(wakeword_models=[WAKE_WORD_MODEL])
    logger.info("wake word model loaded: %s", WAKE_WORD_MODEL)


def listen_for_wake_word() -> None:
    """Block until the wake word score exceeds the configured threshold."""
    if _oww_model is None:
        load_model()
    assert _oww_model is not None

    buf = np.array([], dtype=np.float32)
    gen = audio.get_audio_stream()
    try:
        for chunk in gen:
            buf = np.concatenate([buf, chunk])
            while buf.shape[0] >= _OWW_FRAME_SAMPLES:
                frame = buf[:_OWW_FRAME_SAMPLES]
                buf = buf[_OWW_FRAME_SAMPLES:]
                pcm = np.clip(frame * 32767.0, -32768, 32767).astype(np.int16)
                preds = _oww_model.predict(pcm)
                score = 0.0
                for v in preds.values():
                    if isinstance(v, (int, float)):
                        score = max(score, float(v))
                if score >= WAKE_WORD_THRESHOLD:
                    logger.info("[wake] detected — listening...")
                    return
    finally:
        gen.close()
