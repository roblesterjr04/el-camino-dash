# AI assistant (compute module)

The assistant that runs on the AI card's compute module (a Linux board on the card's 2×20 header; see
[`docs/hardware/ai_card.md`](../../docs/hardware/ai_card.md)). It talks to the card's ESP32-S3 over one UART
([`docs/protocols/ai_link.md`](../../docs/protocols/ai_link.md)); the ESP32 owns CAN, the microphone and the speakers.
Python 3.11+, **standard library only** (the online model needs `pip install anthropic`).

```
card microphone ─► AUDIO ─► end-of-speech ─► speech to text ─► intent engine ──► answer from live data / CAN command
                                             (whisper.cpp)         │ no match
                                                                   ├─► online model (Claude + web search) ─┐ error / slow / refusal
                                                                   └─► on-board model (llama.cpp) ◄────────┘
                                                                           └─ answers UNKNOWN, and online: ask online
                    answer sentences ─► text to speech (piper) ─► SPEECH frames, paced by the card's credit ─► the audio card
```

```bash
./run_tests.sh                         # 50 tests; also compiles the ESP32 side and compares bytes with it
python3 -m elc_ai --config config.example.toml
python3 -m elc_ai --config config.toml --bench sample.wav     # time each stage once
```

| Module | |
| --- | --- |
| `link.py` | The AI link: COBS + CRC-16 frames, parser, message builders |
| `canmsg.py` | The CAN channel messages it reads (SENSOR, SYSTEM, ONLINE, ALERT, AUDIO_CMD) and sends (volume, media, source, sounds) |
| `context.py` | Live car state, units for speech, the prompt snapshot, and the only context allowed to go online (`online_context`) |
| `intents.py` | Deterministic commands and car questions: no model, instant, always works |
| `router.py`, `connectivity.py` | Online or on-board; is the internet there (probe with hysteresis) |
| `endpoint.py` | End-of-speech detection on the 16 kHz stream |
| `stt.py`, `tts.py`, `llm.py` | Backends: whisper.cpp server, piper, llama.cpp server, Claude through the Anthropic SDK |
| `retrieval.py` | BM25 search of the owner's / shop manual for the on-board model |
| `dialog.py` | The orchestrator: one utterance to the spoken answer, with fallback and cancel |
| `agent.py`, `config.py` | The service, the serial port, ALIVE, shutdown, the config file |

## Rules it keeps

- **Audio never leaves the car.** Speech to text is local. Online lookups send the question text, a short history and one
  context line (time, town, units): no sensor data, no position, no audio.
- **The model's output is untrusted.** It can only ask the card to send what the card's allow-list permits
  (`ai_card/src/ai_policy.cpp`): volume, media, source, UI sounds, alerts up to *advisory*. Car state is read, never written.
- **Car questions never go online**, and every question has an on-board answer path when the network is down, slow (no first
  word in `first_token_s`) or the online model declines.
- **Online provider:** Claude through the official `anthropic` SDK with the server-side web search tool; the key is the
  service's `ANTHROPIC_API_KEY` (an `EnvironmentFile`), never on CAN. `model` and `effort` are in `config.toml`; the default
  is `claude-opus-5-5` at `low` effort for short spoken answers. The on-board model is the fallback, so server-side
  fallbacks are not enabled.

## Setting up a module (not yet done on real hardware)

The pieces are standard; none of these steps has been run for this card yet.

1. A Linux image with Wi-Fi and a serial port on header pins 8 / 10 (device-tree overlay for the UART), eMMC, **read-only root**
   with a tmpfs overlay (a power cut must be harmless).
2. Build [`llama.cpp`](https://github.com/ggml-org/llama.cpp) (`llama-server`) and [`whisper.cpp`](https://github.com/ggml-org/whisper.cpp)
   (`whisper-server`) and run both as services on 127.0.0.1 (ports 8080 / 8081 in `config.example.toml`). Start with
   Qwen2.5-1.5B-Instruct Q4_K_M and `ggml-base.en-q5_1`; `--bench` tells you whether a bigger model is affordable.
3. Install [`piper`](https://github.com/rhasspy/piper) and a voice (`en_US-lessac-medium`, 22,050 Hz); set `[tts]`.
4. Copy this directory to `/opt/elc-ai`, `config.example.toml` to `/etc/elc-ai/config.toml`, `deploy/elc-ai.service` to
   `/etc/systemd/system/`. Set `[gpio]` so the service drives ALIVE (header pin 29) high, and low when it stops.
5. Optional: put the manual as `.txt` / `.md` files in `[manual] dir`; for online lookups `pip install anthropic` and put
   `ANTHROPIC_API_KEY=…` in `/etc/elc-ai/env`, then `enabled = true`.
