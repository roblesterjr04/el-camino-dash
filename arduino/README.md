# Arduino

Arduino assets and tooling for the El Camino Dash project live here.

## Layout

- `arduino/sketches/` — sketches by feature/module.
- `arduino/config/arduino-cli.yaml` — reference for the Espressif board index URL (applied by the install script).
- `arduino/scripts/install_arduino_cli.sh` — install `arduino-cli`, register ESP32 + **Earle Philhower RP2040** board indexes, install **arduino:avr** + **esp32:esp32** + **rp2040:rp2040** (arduino-pico) + **arduino:mbed_rp2040** (optional), install **Encoder**, **U8g2** (encoder OLED), and **IRremote** v4 (encoder NEC IR).
- `arduino/scripts/compile_sensor_pack.sh` — compile sensor_pack only.
- `arduino/scripts/upload_sensor_pack.sh` — compile + upload sensor_pack (serial, optional SSH to Pi, optional `--programmer` for ICSP).
- `arduino/scripts/upload_avr_sketch.sh` — generic local compile + upload for any sketch under `arduino/sketches/<name>/` (serial or ICSP).
- `arduino/scripts/upload_esp32_sketch.sh` — compile + upload any ESP32 sketch over **native USB** (default FQBN `esp32:esp32:esp32s3`).
- `arduino/scripts/upload_rp2040_sketch.sh` — compile + upload any RP2040 sketch over **native USB** (default FQBN **`rp2040:rp2040:rpipico`**, Earle Philhower core).
- `arduino/scripts/deploy_encoder_icsp.sh` — compatibility wrapper for `encoder_controller` that now targets Pi Pico USB upload.
- `arduino/scripts/deploy_esp32_audio_usb.sh` — **esp32_audio_bridge** over USB.
- `arduino/scripts/sh_common.inc.sh` — shared helpers (core install check, serial port auto-detect).
- `arduino/scripts/detect_serial_port.py` — used when upload scripts omit the port.
- `arduino/scripts/monitor_sensor_pack.py` — live decoder for the sensor pack UART (v4 frames + text lines).

The sensor pack streams binary frames over UART to the three ESP32-P4 displays; the format is in [`docs/protocols/sensor_uart.md`](../docs/protocols/sensor_uart.md).

## Quick Start (macOS + Homebrew)

Install CLI, cores, and Encoder library:

```bash
bash arduino/scripts/install_arduino_cli.sh
```

Compile sensor_pack:

```bash
bash arduino/scripts/compile_sensor_pack.sh
```

Upload sensor_pack (USB serial). With no port argument, picks the first suitable USB serial device (skips Bluetooth):

```bash
bash arduino/scripts/upload_sensor_pack.sh
bash arduino/scripts/upload_sensor_pack.sh /dev/cu.usbmodem101 arduino:avr:uno
```

## Encoder Controller on Pi Pico

Use the **Earle Philhower** core (**Tools → Board → Raspberry Pi Pico**, package *Raspberry Pi Pico/RP2040*), not **Arduino Mbed OS RP2040 Boards**, unless you have a reason. On **Mbed**, a repeating **four long + four short** green LED blink is a [known RTOS fault indicator](https://github.com/arduino/ArduinoCore-mbed/issues/316); switch to **arduino-pico** and re-flash.

Flash `encoder_controller` (defaults to **`rp2040:rp2040:rpipico`**; auto-selects serial port):

```bash
bash arduino/scripts/install_arduino_cli.sh
bash arduino/scripts/deploy_encoder_icsp.sh
bash arduino/scripts/upload_rp2040_sketch.sh encoder_controller /dev/cu.usbmodem1101
```

To build with Mbed explicitly: `--fqbn arduino:mbed_rp2040:pico`.

## AVR over ICSP (USB programmer)

Use `arduino-cli upload --programmer …` with the port your programmer expects (often the literal `usb` for **usbtinyisp** / **usbasp**).

**sensor_pack** (existing script):

```bash
bash arduino/scripts/upload_sensor_pack.sh usb arduino:avr:uno --programmer usbtinyisp
```

**encoder_controller** (wrapper). With no arguments, uses **usbtinyisp**, port **usb**, **arduino:avr:uno**:

```bash
bash arduino/scripts/deploy_encoder_icsp.sh
bash arduino/scripts/deploy_encoder_icsp.sh usb arduino:avr:uno --programmer usbtinyisp
```

**Any sketch** (generic):

```bash
bash arduino/scripts/upload_avr_sketch.sh encoder_controller usb arduino:avr:uno --programmer usbtinyisp
```

Programmer IDs match what **avrdude** / Arduino expect (e.g. `usbtinyisp`, `usbasp`, `stk500v2`). Use `arduino-cli upload --help` and your programmer’s docs.

## ESP32 over native USB

Generic uploads default to **ESP32-S3** (`esp32:esp32:esp32s3`) in `upload_esp32_sketch.sh` when you omit `--fqbn`.

**esp32_audio_bridge** is different: it uses **Bluetooth Classic A2DP + AVRCP**, which requires a **classic ESP32** (`esp32:esp32:esp32`), not ESP32-S3 / C3 / S2 (those do not support BR/EDR audio). Install **ESP32-A2DP** and **arduino-audio-tools** via `install_arduino_cli.sh` (git URLs).

Flash **esp32_audio_bridge** (BT media hub; the center display reads now-playing over I2C 0x2A). With no arguments, auto-selects a serial port and uses `esp32:esp32:esp32`. The pinned PlatformIO build in `firmware/esp32_classic_bt_audio` is preferred:

```bash
bash arduino/scripts/deploy_esp32_audio_usb.sh
bash arduino/scripts/deploy_esp32_audio_usb.sh /dev/cu.usbmodem1101
```

Other boards (only if you have a different sketch that does not need A2DP):

```bash
bash arduino/scripts/upload_esp32_sketch.sh esp32_audio_bridge /dev/cu.usbmodem1101 --fqbn esp32:esp32:esp32s3
```

Generic upload:

```bash
bash arduino/scripts/upload_esp32_sketch.sh other_sketch /dev/cu.usbmodem1101 --fqbn esp32:esp32:esp32s3
```

## Other

Monitor sensor pack serial:

```bash
python3 arduino/scripts/monitor_sensor_pack.py --port /dev/cu.usbmodem101 --baud 115200
```

## Notes

- Default AVR FQBN in many scripts is `arduino:avr:uno`; use `arduino:avr:nano` (or your exact board) when needed.
- To discover ports: `arduino-cli board list`.
- To discover board IDs: `arduino-cli board listall`.
