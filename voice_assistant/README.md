# El Camino AI Voice Assistant

Python voice pipeline: wake word → record (Silero VAD) → faster-whisper → Claude or Ollama → Piper TTS. Intended for `/opt/el-camino-dash/voice_assistant` on the gauge-cluster image (or `~/elcamino/voice_assistant` if you mirror it there).

## Hardware requirements

- Raspberry Pi 5
- USB microphone (see ALSA `arecord -l`)
- Arduino Mega sending JSON telemetry on `/dev/ttyUSB0` (115200 baud)
- LTE or Wi‑Fi for cloud LLM (optional; Ollama offline fallback)

## Software installation (Debian / Raspberry Pi OS)

Install system packages (also applied via the `el-camino-dash` image hook `10-install-packages.sh`):

```bash
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  python3 python3-pip python3-dev build-essential \
  ffmpeg alsa-utils pulseaudio-utils \
  portaudio19-dev libportaudio2 libsndfile1
```

Install Python dependencies with **system Python** (no venv), using PEP 668 override on Bookworm:

```bash
cd /opt/el-camino-dash/voice_assistant
sudo python3 -m pip install --break-system-packages --no-cache-dir -r requirements.txt
```

Set the Anthropic API key (Claude) when using cloud mode:

```bash
export ANTHROPIC_API_KEY="your-key"
```

Optional: `LOG_LEVEL=DEBUG` for verbose logs.

## Piper TTS (standalone binary)

Piper is **not** a Python package. Download a release from [Piper releases](https://github.com/rhasspy/piper/releases) for `linux-aarch64` (Pi 5), extract the `piper` binary to a directory on `PATH` or `/home/pi/piper/piper`, and match `PIPER_EXECUTABLE` / `PIPER_MODEL_PATH` in `config.py`.

Download the **en_US-lessac-medium** voice and config into `~/piper/`:

```bash
mkdir -p ~/piper
cd ~/piper
curl -LO https://huggingface.co/rhasspy/piper-voices/resolve/v1.0.0/en/en_US/lessac/medium/en_US-lessac-medium.onnx
curl -LO https://huggingface.co/rhasspy/piper-voices/resolve/v1.0.0/en/en_US/lessac/medium/en_US-lessac-medium.onnx.json
chmod +x ~/piper/piper   # after extracting the binary next to the models
```

Adjust `config.py` if you use different paths.

## Ollama (offline fallback)

```bash
curl -fsSL https://ollama.com/install.sh | sh
ollama pull qwen2.5:1.5b
```

Ensure `OLLAMA_BASE_URL` in `config.py` matches your daemon (default `http://localhost:11434`).

## Switching audio output

Edit `config.py`:

- **`AUDIO_OUTPUT_BACKEND`**: `"alsa"` (default `aplay`), `"i2s"` (`sounddevice` playback), or `"pulse"` (`paplay`).
- **`ALSA_OUTPUT_DEVICE`**: e.g. `"default"`, `"hw:1,0"` for a specific card.

For I2S DAC HATs, configure `/boot/firmware/config.txt` (example — adjust overlay to your board):

```text
dtoverlay=googlevoicehat-soundcard
# or: dtoverlay=hifiberry-dac
```

Reboot after `config.txt` changes. Then set `AUDIO_OUTPUT_BACKEND="i2s"` and the correct `ALSA_OUTPUT_DEVICE` / sounddevice device name if needed.

## Run manually

```bash
cd /opt/el-camino-dash/voice_assistant
sudo -E PYTHONPATH=. python3 main.py
```

(`PYTHONPATH=.` is only needed if the current directory is not already the package root.)

## systemd service

Install `deploy/systemd/elcamino-voice.service` to `/etc/systemd/system/` and create `/etc/elcamino-voice.env`:

```bash
sudo install -m 0644 deploy/systemd/elcamino-voice.service /etc/systemd/system/
sudo tee /etc/elcamino-voice.env <<'EOF'
ANTHROPIC_API_KEY=your-key-here
LOG_LEVEL=INFO
EOF
sudo systemctl daemon-reload
sudo systemctl enable --now elcamino-voice.service
```

The unit starts after `network.target` so basic connectivity is up before Claude is used.

## Arduino serial format

The Mega should emit **one JSON object per line** at 115200 baud, for example:

```json
{"rpm": 1200, "speed_mph": 35, "coolant_f": 195, "oil_psi": 45, "voltage": 14.1, "faults": []}
```

Fields:

| Field        | Type    | Meaning              |
|-------------|---------|----------------------|
| `rpm`       | number  | Engine RPM           |
| `speed_mph` | number  | Speed (mph)          |
| `coolant_f` | number  | Coolant °F           |
| `oil_psi`   | number  | Oil pressure (PSI)   |
| `voltage`   | number  | Battery voltage (V)  |
| `faults`    | array   | Diagnostic strings   |

Missing fields or a closed serial port yield `"Vehicle data unavailable"` in the LLM prompt.
