"""Text-to-speech via Piper CLI."""

from __future__ import annotations

import logging
import subprocess

from config import PIPER_EXECUTABLE, PIPER_MODEL_PATH, TTS_OUTPUT_PATH

logger = logging.getLogger(__name__)


class TTSError(RuntimeError):
    """Raised when Piper exits with a non-zero status."""


def synthesize(text: str) -> str:
    logger.info("[tts] synthesizing %s chars", len(text))
    cmd = [PIPER_EXECUTABLE, "--model", PIPER_MODEL_PATH, "--output_file", TTS_OUTPUT_PATH]
    proc = subprocess.run(
        cmd,
        input=text.encode("utf-8"),
        capture_output=True,
    )
    if proc.returncode != 0:
        err = proc.stderr.decode("utf-8", errors="replace").strip()
        raise TTSError(f"Piper failed ({proc.returncode}): {err}")
    return TTS_OUTPUT_PATH
