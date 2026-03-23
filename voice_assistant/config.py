"""Configuration constants for the El Camino voice assistant."""

import logging
import os

# Audio input
MIC_DEVICE_INDEX = None  # None = system default; set to int to specify
MIC_SAMPLE_RATE = 16000
MIC_CHANNELS = 1
MIC_CHUNK_DURATION_MS = 32

# Audio output backend — "alsa" | "i2s" | "pulse"
AUDIO_OUTPUT_BACKEND = "alsa"
ALSA_OUTPUT_DEVICE = "default"
OUTPUT_SAMPLE_RATE = 22050

# Wake word
WAKE_WORD_MODEL = "hey_jarvis"
WAKE_WORD_THRESHOLD = 0.5

# STT
WHISPER_MODEL = "base.en"
WHISPER_DEVICE = "cpu"
WHISPER_COMPUTE_TYPE = "int8"

# VAD
VAD_SILENCE_THRESHOLD_MS = 1200
VAD_MAX_RECORDING_S = 15

# LLM
CLAUDE_MODEL = "claude-haiku-4-5"
CLAUDE_MAX_TOKENS = 300
OLLAMA_MODEL = "qwen2.5:1.5b"
OLLAMA_BASE_URL = "http://localhost:11434"
NETWORK_CHECK_HOST = "8.8.8.8"
NETWORK_CHECK_PORT = 53
NETWORK_CHECK_TIMEOUT_S = 3

# TTS
PIPER_MODEL_PATH = "/home/pi/piper/en_US-lessac-medium.onnx"
PIPER_EXECUTABLE = "/home/pi/piper/piper"
TTS_OUTPUT_PATH = "/tmp/tts_response.wav"

# Telemetry
TELEMETRY_SERIAL_PORT = "/dev/ttyUSB0"
TELEMETRY_BAUD_RATE = 115200
TELEMETRY_TIMEOUT_S = 1.0
TELEMETRY_UPDATE_INTERVAL_S = 0.5

SYSTEM_PROMPT_TEMPLATE = """You are an AI assistant built into a 1964 El Camino.
Keep all responses under 2 sentences — they will be spoken aloud.
Be concise and direct. Avoid lists. Speak naturally.

Current vehicle data:
{telemetry}"""

LOG_LEVEL = os.environ.get("LOG_LEVEL", "INFO")


def configure_logging() -> None:
    level = getattr(logging, LOG_LEVEL.upper(), logging.INFO)
    logging.basicConfig(
        level=level,
        format="%(levelname)s %(name)s: %(message)s",
    )
