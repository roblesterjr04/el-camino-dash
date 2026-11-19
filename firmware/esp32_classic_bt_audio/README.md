# esp32_classic_bt_audio: the audio card / Bluetooth bridge build

The PlatformIO project for the audio firmware. It holds no sources of its own: `src_dir` points at
[`arduino/sketches/esp32_audio_bridge`](../../arduino/sketches/esp32_audio_bridge/), and this directory adds the
pinned platform and libraries, the [`elc_can`](../elc_can/) library (which turns CAN on in the sketch), one env per
board, the flash script and the host tests.

The firmware is two things on one classic ESP32:

- **The audio sink** (channel protocol node type 5): one 48 kHz output that plays the source the console picks
  (Bluetooth, the radio card's audio on the I2S rail's RET, or off) through a 10-band equaliser and the master
  volume, with calls, a voice card and the alert chimes mixed over it, then fader and balance to the front and rear
  DACs. It answers AUDIO_CMD (STATUS, SOURCE, VOLUME, MEDIA, PLAY / STOP of the chimes) and NET RAIL, and has a menu
  page (chime volume, fader, balance, the Equalizer).
- **The Bluetooth bridge** (heartbeat node type 1): A2DP sink and AVRCP from a phone, HFP hands-free calls, pairing
  with numeric comparison and up to 8 paired phones; now playing, transport, album art and phone state in its node's
  CAN range, phone commands from `0x215`.

Overview of all the firmware: [`../README.md`](../README.md). The card itself: [`AudioCarrier/`](../../AudioCarrier/).

```bash
./flash.sh --upload-port /dev/cu.usbserial-…                    # esp32wrover (DevKitC-VE), the default
ENV=audiocarrier ./flash.sh --upload-port /dev/cu.usbserial-…   # the audio card
test/host/run_tests.sh                                          # chimes, DSP and sink tests on the Mac
```

## Status

- Host tests pass for the chimes, the DSP (equaliser, resampler, decimator, ring, fader) and the sink (sources,
  volume, MEDIA, RET arbitration, the page): `test/host/run_tests.sh`.
- The root [`README.md`](../../README.md#status): the Bluetooth bridge runs on a DevKitC; the I2S rail and the aux
  input are untested on hardware. [`AudioCarrier/README.md`](../../AudioCarrier/README.md) lists what has not been
  tried on a built card: I2S1 as a slave on I2S0's clock pins, RET from a radio card, the electret microphone path.
- There is no aux input in this firmware: `sink.h` and `audio_engine.h` say so, and `sink.cpp` refuses source 3.

## Hardware it targets

Classic ESP32 only: A2DP and HFP need Bluetooth BR/EDR, which the ESP32-S3 / C3 / S2 do not have.

| Env | Board | Notes |
| --- | --- | --- |
| `esp32wrover` (default in `flash.sh`) | ESP32-DevKitC-VE (WROVER-E, 8 MB flash, 8 MB PSRAM) | album-art buffers in PSRAM, 128×128 small copy; INMP441 bench microphone on GPIO35 |
| `audiocarrier` | the audio card ([`AudioCarrier/`](../../AudioCarrier/), a DevKitC-VE in sockets) | node from the slot ID pin; PCM1808 ADC, front and rear DACs, the I2S rail |
| `esp32dev` | ESP32-DevKitC-32 (WROOM, no PSRAM) | 64×64 small art copy, usually skipped for lack of RAM |

Pins (build flags in `platformio.ini`, defaults in `esp32_audio_bridge.ino`):

| Function | `esp32dev` / `esp32wrover` | `audiocarrier` |
| --- | --- | --- |
| CAN TX / RX (`ELC_CAN_TX_PIN` / `_RX_PIN`) | 32 / 33 | 32 / 33 |
| Node straps, rev 1 carrier (`ELC_CAN_SLOT_ID0_PIN` / `_ID1_PIN`) | 13 / 23 | 13 / 22 (set, but not read: with a slot ID pin the slot decides) |
| Slot ID pin, ADC (`ELC_CAN_CARD_SLOT_PIN`) | -1 (not a slot card) | 36 |
| I2S BCK / WS / front DOUT (`I2S_PIN_BCK` / `_WS` / `_DATA`) | 14 / 25 / 27 | 14 / 25 / 27 |
| Microphone ADC in (`I2S_PIN_DIN`) | -1 / 35 | 35 |
| MCLK (`I2S_PIN_MCLK`) | -1 | 0 (12.288 MHz) |
| Rear DAC out (`I2S_PIN_REAR_DOUT`) | -1 | 26 |
| Rail RET in (`I2S_PIN_RET_DIN`) | -1 | 34 |
| Rail buffer enable, low = on (`RAIL_ENABLE_PIN`) | -1 | 21 |
| DAC soft mute, high = play (`DAC_UNMUTE_PIN`) | -1 | 4 |

The sketch stops the build with `#error` if an I2S pin is a CAN pin.

## Layout

The sources are in [`arduino/sketches/esp32_audio_bridge`](../../arduino/sketches/esp32_audio_bridge/):

| File | Role |
| --- | --- |
| `esp32_audio_bridge.ino` | Setup and loop: Bluetooth (A2DP, AVRCP metadata and cover art), CAN (node number, heartbeat, the bridge's frames, the channel protocol as the audio sink), the serial console |
| `sink.{h,cpp}` | The AUDIO_CMD sink: source, master volume, MEDIA forwarding, RET arbitration (NET RAIL), STATUS, the card's UI page. No Arduino code; host-tested |
| `audio_engine.{h,cpp}` | The output engine task (core 1): I2S at 48 kHz with 32-bit slots, music + voice + chimes, fader / balance, front and rear DACs, the microphone and RET in, the rail buffer |
| `audio_dsp.{h,cpp}` | `Eq10`, `Resampler`, `Decimator`, `FrameRing`. No Arduino code; host-tested |
| `chimes.{h,cpp}` | The alert chimes (sounds 2, 3, 4), synthesised. No Arduino code; host-tested |
| `phone.{h,cpp}` | HFP client, pairing (numeric comparison, pairing window), the paired list in NVS |
| `art_variants.{h,cpp}` | Smaller copies of the cover for CAN: a small JPEG and a 48×48 4-bit grey image (JPEGDEC + JPEGENC) |

Here:

| File | Role |
| --- | --- |
| `platformio.ini` | The three envs, pinned platform (pioarduino `55.03.312-1`) and libraries (ESP32-A2DP, arduino-audio-tools, JPEGDEC, JPEGENC at fixed commits), `huge_app.csv` partitions |
| `flash.sh` | Builds and uploads `ENV` (default `esp32wrover`) with the display firmware's venv |
| `test/host/run_tests.sh`, `test_chimes.cpp`, `test_audio_dsp.cpp`, `test_sink.cpp` | Host tests; `run_tests.sh --wav <dir>` also writes each chime as a 44.1 kHz WAV |

## Build, flash, test

`flash.sh` uses PlatformIO from `../esp32_p4_display/.venv` and stops if it does not exist: run
`../esp32_p4_display/flash.sh` once first (it creates the venv), or make one with
`python3 -m venv ../esp32_p4_display/.venv && ../esp32_p4_display/.venv/bin/pip install -r ../esp32_p4_display/requirements.txt`.
It removes the cached `elc_can` copy from `.pio/libdeps` before each build.

```bash
./flash.sh --upload-port /dev/cu.usbserial-…                       # esp32wrover
ENV=audiocarrier ./flash.sh --upload-port /dev/cu.usbserial-…      # the audio card
ENV=esp32dev ./flash.sh --upload-port /dev/cu.usbserial-…          # WROOM DevKitC-32
../esp32_p4_display/.venv/bin/python -m platformio run -e audiocarrier   # build only
test/host/run_tests.sh
```

The plain arduino-cli build (`arduino/scripts/deploy_esp32_audio_usb.sh`) still compiles the sketch, but without
`elc_can` it has no CAN, so nothing reads its now playing ([`arduino/README.md`](../../arduino/README.md)).

## Configuration

| Flag | Default | Meaning |
| --- | --- | --- |
| `BOARD_HAS_PSRAM` | set by `esp32wrover`, `audiocarrier` | PSRAM buffers, 128 px small art |
| `BT_DEVICE_NAME` | `"ElCamino-Media"` | the name phones see |
| `AUDIO_BRIDGE_SERIAL_METADATA` | 0 | 1 prints the AVRCP line on the serial port |
| `MIC_RIGHT_CHANNEL` | 0 | take the microphone from the right channel |
| CAN and I2S pins | see *Hardware* | |

NVS: namespace `bridge` (`sink`: the sink's settings as one blob; `mvol`, `chvol`: the volumes kept before the sink
existed), namespace `phones` (the paired list, beside Bluedroid's own bond store).

Serial console (115200; `help`): `phone`, `pair [on|off]`, `confirm yes|no`, `answer`, `end`, `mute [on|off]`,
`vol 0-15`, `connect <n>`, `disconnect`, `forget <n>`, `redial`, `voice [on|off]`, `waiting 0|1|2`,
`route car|phone`, `play`, `next`, `prev`, `chime 2|3|4 [repeat]`, `chime stop`, `audio` (the engine and the sink),
`source bt|radio|off|next`.

## How it talks to the rest of the system

- **Node number.** On the audio card (`ELC_CAN_CARD_SLOT_PIN` set) from the slot ID pin: `0x14 + slot - 1`; with no
  main board pulling the pin down it takes `0x14`. On a rev 1 carrier from the two straps: `0x10` + (0..2).
  The range index (node - `0x10`) picks its message range `0x400 + 0x40 · index`.
- **Cluster frames** ([`can_bus.md`](../../docs/protocols/can_bus.md#audio-bridge-node-type-1)): transport (4 Hz),
  text, art info and data (answering `0x211`), phone status, phone text, pairing; it takes `0x211` art requests and
  `0x215` phone commands. Heartbeat node type 1.
- **Channels** ([`can_channels.md`](../../docs/protocols/can_channels.md)): ANNOUNCE as the audio sink (type 5,
  product `0x0004` audio card or `0x0002` audio bridge), produces and consumes AUDIO_CMD and UI; NET RAIL for RET;
  its page at the fixed `ELC_SINK_ITEM_*` numbers. The console follows the first node whose heartbeat says audio
  bridge ([`esp32s3_console/README.md`](../esp32s3_console/README.md)).
- **I2S rail** (the audio card): [`docs/hardware/accessory_ports.md`](../../docs/hardware/accessory_ports.md) and
  [`can_channels.md`](../../docs/protocols/can_channels.md#the-i2s-rail).
- **Bench:** the USB CAN link (`tools/can_relay.py`, [`usb_link.md`](../../docs/protocols/usb_link.md)); the relay's
  simulated audio card goes quiet when this firmware announces.

## Extending it

- **A new chime or built-in sound:** add it to `chimes.{h,cpp}` with its length, to the *Sounds* registry in
  `can_channels.md`, to `tools/cards_sim.py` (it uses the same lengths), and a case to `test/host/test_chimes.cpp`.
- **A new item on the card's page:** the item numbers are fixed in `elc_chan_msg.h` (`ELC_SINK_ITEM_*`); add the
  number there, describe and apply it in `sink.cpp`, document it in `can_channels.md` (*The sink's page*), and test
  it in `test/host/test_sink.cpp`.
- **A new phone command:** the numbers are the CAN protocol's (`ElcCanPhoneCommand` in `elc_can_codec.h`,
  `can_bus.md` *Phone*); `phone.h` keeps its own copy so the arduino-cli build works without `elc_can`, and the
  sketch checks they match.

## Known issues

- [`../README.md`](../README.md) and [`can_channels.md`](../../docs/protocols/can_channels.md) list "aux in" among the
  sink's sources; this firmware has none (`sink.h`, `audio_engine.h`).
- `art_variants.h` says the small copy is 64×64; it is 128×128 with PSRAM (the same header, and `can_bus.md`).
- `chimes.h` refers to `call_audio.cpp`, which does not exist.
- `AudioCarrier/README.md` says the firmware ignores the node straps "when it reads a valid slot"; the code does not
  read them at all when `ELC_CAN_CARD_SLOT_PIN` is set (no slot read = node `0x14`).
