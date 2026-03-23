"""El Camino AI voice assistant — main pipeline."""

from __future__ import annotations

import logging
import os
import tempfile
import wave

import numpy as np
import torch

from config import (
    MIC_CHUNK_DURATION_MS,
    MIC_SAMPLE_RATE,
    VAD_MAX_RECORDING_S,
    VAD_SILENCE_THRESHOLD_MS,
    WAKE_WORD_MODEL,
    configure_logging,
)

configure_logging()

import audio  # noqa: E402
import llm  # noqa: E402
import stt  # noqa: E402
import telemetry  # noqa: E402
import tts  # noqa: E402
import wake_word  # noqa: E402
from silero_vad import VADIterator, get_speech_timestamps, load_silero_vad  # noqa: E402, F401

logger = logging.getLogger(__name__)


def _write_tone_wav(path: str, *, freq_hz: float, duration_s: float, sample_rate: int) -> None:
    t = np.linspace(0.0, duration_s, int(sample_rate * duration_s), endpoint=False, dtype=np.float64)
    sine = (0.2 * np.sin(2.0 * np.pi * freq_hz * t)).astype(np.float32)
    pcm = (np.clip(sine, -1.0, 1.0) * 32767.0).astype(np.int16)
    with wave.open(path, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(pcm.tobytes())


def record_until_speech_ends(vad_model) -> np.ndarray:
    """Record from mic until Silero VAD reports end of speech or max duration."""
    chunk_samples = int(MIC_SAMPLE_RATE * (MIC_CHUNK_DURATION_MS / 1000.0))
    max_samples = int(MIC_SAMPLE_RATE * VAD_MAX_RECORDING_S)
    vad_iter = VADIterator(
        vad_model,
        threshold=0.5,
        sampling_rate=MIC_SAMPLE_RATE,
        min_silence_duration_ms=VAD_SILENCE_THRESHOLD_MS,
    )
    chunks: list[np.ndarray] = []
    total = 0
    end_at: int | None = None
    gen = audio.get_audio_stream()
    try:
        for chunk in gen:
            if chunk.shape[0] < chunk_samples:
                chunk = np.pad(chunk, (0, chunk_samples - chunk.shape[0]))
            elif chunk.shape[0] > chunk_samples:
                chunk = chunk[:chunk_samples]
            chunks.append(chunk.astype(np.float32, copy=False))
            total += chunk.shape[0]
            x = torch.from_numpy(chunk).float().unsqueeze(0)
            out = vad_iter(x)
            if out and "end" in out:
                end_at = int(out["end"])
                break
            if total >= max_samples:
                break
    finally:
        gen.close()

    if not chunks:
        return np.array([], dtype=np.float32)

    audio_out = np.concatenate(chunks, axis=0)
    if end_at is not None:
        audio_out = audio_out[: min(end_at, len(audio_out))]
    elif total >= max_samples:
        audio_out = audio_out[:max_samples]
    return np.asarray(audio_out, dtype=np.float32)


def main() -> None:
    print("El Camino AI Assistant starting...")
    audio.log_active_backend()
    telemetry.start()
    wake_word.load_model()
    vad_model = load_silero_vad()
    logger.info('[main] ready — say "%s" to activate', WAKE_WORD_MODEL)

    while True:
        wake_word.listen_for_wake_word()
        with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as tmp:
            tone_path = tmp.name
        try:
            _write_tone_wav(tone_path, freq_hz=880.0, duration_s=0.2, sample_rate=MIC_SAMPLE_RATE)
            audio.play_audio(tone_path)
        finally:
            try:
                os.unlink(tone_path)
            except OSError:
                pass

        speech = record_until_speech_ends(vad_model)
        text = stt.transcribe(speech)
        if not text or not text.strip():
            continue
        logger.info('[main] query: "%s"', text)
        telem = telemetry.get_telemetry()
        telem_str = telemetry.format_telemetry_for_prompt(telem)
        response = llm.query(text, telem_str)
        logger.info('[main] response: "%s"', response)
        try:
            wav_path = tts.synthesize(response)
            audio.play_audio(wav_path)
        except tts.TTSError as exc:
            logger.warning("TTS failed: %s", exc)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        logging.getLogger(__name__).info("Shutting down.")
