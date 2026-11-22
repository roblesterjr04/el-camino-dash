# AI card front end (ESP32-S3)

Firmware for the AI voice card's **ESP32-S3** ([`AICard/`](../../AICard/README.md); requirements in
[`docs/hardware/ai_card.md`](../../docs/hardware/ai_card.md)). It is the always-on half of the card: CAN node
`0x14 + slot − 1` (node type 8, product `0x0007`), the I2S rail as a slave (the microphone in, the voice out on RET), the
wake word, the power switch of the compute module, and the rules for what the assistant may do. The assistant itself
runs on the module: [`../ai_agent/`](../ai_agent/README.md). The link between them: [`docs/protocols/ai_link.md`](../../docs/protocols/ai_link.md).

```bash
test/host/run_tests.sh                 # the logic, on the host, with ASan / UBSan: no toolchain needed
pio run                                # the firmware (see "Status")
pio run -e ai-card-keyonly             # bring-up build: press-to-talk only (no wake-word model), bench node 0x14
```

| File | |
| --- | --- |
| `src/ai_link.{h,cpp}` | The AI link framing: COBS + CRC-16, byte-stream parser, payload helpers. The same bytes as `ai_agent/elc_ai/link.py` (the agent's tests compile this file and compare) |
| `src/ai_audio.{h,cpp}` | Rate conversion (rail 44.1 / 48 kHz ↔ speech 16 / 22.05 / 24 kHz, polyphase windowed-sinc), 32-bit stereo ↔ 16-bit mono, level meter, the speech pacer (how much the module may send) |
| `src/ai_policy.{h,cpp}` | **What the assistant may put on CAN**: allow-list per channel and opcode, severity ≤ advisory, key allow-list, rate limits. Everything else is refused |
| `src/ai_power.{h,cpp}` | Power sequencer for the module: off / booting / running / stopping / fault, thermal limits with hysteresis, hung-module restart, NTC conversion |
| `src/ai_session.{h,cpp}` | One voice session: wake word or key → listen → think → ask the audio card for RET → speak → release; every timeout and failure path |
| `src/ai_ui.{h,cpp}` | The card's page in the dash menu (settings and status over the UI channel) |
| `src/ai_commands.{h,cpp}` | The few voice commands the ESP32 recognises itself (volume, mute, next / previous, pause), as AUDIO_CMD transfers |
| `src/ai_pins.h` | Pin map (the schematic generator is the source of truth) |
| `src/wake_engine.{h,cpp}` | esp-sr WakeNet binding (`AI_WAKE_ENGINE=1`) |
| `src/main.cpp` | Hardware glue: CAN (`elc_can`), I2S slave, UART link, GPIO, NVS |
| `partitions.csv` | 16 MB: two app slots, NVS, the `model` partition for esp-sr, storage |

## Status

- **Tested (host): everything in `ai_*.cpp`** (1,772 checks): framing against the agent's Python, the resampler (tone level,
  alias rejection, chunk independence), the allow-list, every power-sequencer path, every session path, the menu page.
- **Not built and not run: `main.cpp` and `wake_engine.cpp`.** The PlatformIO registry could not be reached when they
  were written, so the first build will probably need small fixes (the ESP-IDF 5 `i2s_std.h` calls, the esp-sr names).
  Bring up in this order: `ai-card-keyonly`, the console (`Serial` at 115200), CAN heartbeat and ANNOUNCE, the menu page,
  the link with a laptop playing the module (`python3 -m elc_ai` on the bench with a USB-serial adapter), the rail, then the
  wake word.
- **Not written:** the MultiNet command recogniser binding (the phrases and their CAN commands are in `ai_commands.cpp`),
  the ONLINE-channel decoders in `elc_can` (the agent decodes them itself, from the spec), firmware update over BULK, and
  barge-in with echo cancellation.
- **Open protocol items:** the `zones:4 | priority:4` byte of AUDIO_CMD PLAY is read as priority = low nibble. The audio
  card's sink firmware does not exist yet; check it against the nibble order when it does.
