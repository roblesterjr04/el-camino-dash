# Sketches

Arduino sketches, one folder each. The product firmware is the PlatformIO code in [`firmware/`](../../firmware/); of
the sketches here, only the audio bridge is still part of it. The Raspberry Pi version of the dash that the encoder
controller was written for is shelved at git tag `pi-pygame-final`.

| Sketch | Board | Status |
| --- | --- | --- |
| [`esp32_audio_bridge/`](esp32_audio_bridge/) | Classic ESP32 (ESP32-DevKitC-VE, WROVER-E; or the WROOM DevKitC-32) | **Current.** The audio card's firmware, built by [`firmware/esp32_classic_bt_audio`](../../firmware/esp32_classic_bt_audio/) |
| [`sensor_pack/`](sensor_pack/) | ATmega328P (Arduino Uno / Nano class) | **Rev 1 only.** Superseded on the rev 2 main board by the sensor card firmware |
| [`encoder_controller/`](encoder_controller/) | RP2040 (Raspberry Pi Pico); also builds for an ATmega328P | **Legacy.** The Pi-era knobs / IR / OLED controller, replaced by the ESP32-S3 console |

Helper scripts for `arduino-cli` (install, compile, upload) are described in [`../README.md`](../README.md).

## esp32_audio_bridge

The audio card: the cluster's audio sink and Bluetooth bridge, on a classic ESP32 (Bluetooth Classic A2DP exists on
the original ESP32 only, not on the S3 / C3 / S2).

- The audio sink: one 48 kHz output playing the source the console picks (Bluetooth, the radio card's audio on the
  I2S rail's RET, or nothing) through a 10-band equaliser and master volume, with fader and balance across the front
  and rear DACs. AUDIO_CMD and NET RAIL on CAN, and the card's menu page. The sink refuses aux in as a source
  (`sink.h`).
- A2DP sink and AVRCP metadata (title, artist, album, track, duration); album art served on CAN.
- HFP hands-free calls, pairing with numeric comparison, the paired-phone list.
- Alert chimes mixed over the music or a call.

CAN is its only link to the displays. Its node is `0x14 + slot - 1` on the rev 2 audio card, or `0x10` plus its two
straps on a rev 1 carrier ([`docs/protocols/can_bus.md`](../../docs/protocols/can_bus.md),
[`docs/protocols/can_channels.md`](../../docs/protocols/can_channels.md)).

| File | |
| --- | --- |
| `esp32_audio_bridge.ino` | Setup, the loop, Bluetooth callbacks, CAN, the serial console (`help`) |
| `sink.{h,cpp}` | The audio sink's state and its CAN side (AUDIO_CMD, RAIL, the menu page) |
| `audio_engine.{h,cpp}` | The output task: I2S at 48 kHz, the music source, call audio and the microphone, chimes mixed in |
| `audio_dsp.{h,cpp}` | The 10-band equaliser and the resampler |
| `chimes.{h,cpp}` | The built-in alert sounds 2–4, synthesised |
| `phone.{h,cpp}` | HFP calls, pairing, the paired list |
| `art_variants.{h,cpp}` | Smaller copies of the phone's cover art for CAN (64 × 64 JPEG for the center, 48 × 48 grey for the console) |

Build and flash with PlatformIO, which pins every library version and adds the CAN library:

```bash
firmware/esp32_classic_bt_audio/flash.sh --upload-port /dev/cu.usbmodem…
```

`ENV=audiocarrier` builds for the audio card, `ENV=esp32dev` for the WROOM DevKitC-32; the default is `esp32wrover`
(DevKitC-VE). Host tests: `firmware/esp32_classic_bt_audio/test/host/run_tests.sh`. A plain `arduino-cli` build
(`arduino/scripts/deploy_esp32_audio_usb.sh`) has no CAN, so nothing on the cluster hears it.

## sensor_pack

The rev 1 sensor pack: an ATmega328P that reads the fuel, coolant, oil and ambient senders, vehicle speed (hall
sensor), engine speed (coil), ignition and the alternator's L terminal, keeps the odometer in an MB85RC FRAM at I2C
`0x50`, and sends a 36-byte binary frame at 115200 baud from one TX pin to all three displays
([`docs/protocols/sensor_uart.md`](../../docs/protocols/sensor_uart.md)). Pin assignments and the sender wiring are in
the comment block at the top of `sensor_pack.ino`.

It belongs to the first ESP32-P4 main board ([`DigiDash/`](../../DigiDash/)). On the rev 2 board the sensor card
replaces it: [`firmware/sensor_card`](../../firmware/sensor_card/) (ATmega2560, rev A) and
[`firmware/sensor_card_stm32`](../../firmware/sensor_card_stm32/) (STM32G0B1, rev B), CAN node `0x04`. The sensor card
still sends the same 36-byte serial frame as a fallback, and rev A keeps the odometer in the sensor pack's FRAM slots,
so a stored mileage carries over.

```bash
bash arduino/scripts/upload_sensor_pack.sh /dev/cu.usbmodem… arduino:avr:uno
python3 arduino/scripts/monitor_sensor_pack.py --port /dev/cu.usbmodem…     # decode the frames on the bench
```

D11 HIGH selects the demo sweep, and the pin has no pull resistor: tie it LOW in the harness (or fit a pull-down), or
the gauges can show fake data. When the frame format changes, the sensor pack and the three displays must be flashed
together.

## encoder_controller

The knob and OLED controller from the Raspberry Pi version: two rotary encoders, an NEC IR receiver and a 256 × 64
SSD1322 OLED on SPI, talking to the Pi over a 115200-baud UART with newline-delimited text frames
([`docs/protocols/nano_uart.md`](../../docs/protocols/nano_uart.md)). It targets the Raspberry Pi Pico with the Earle
Philhower `arduino-pico` core; the code also keeps an ATmega328P path.

Nothing in the ESP32 firmware talks to it. Its role is now the console board's
([`firmware/esp32s3_console`](../../firmware/esp32s3_console/), ESP32-S3 on CAN, node `0x13`). The OLED drawing can be
previewed with the legacy [`tools/oled_sim`](../../tools/oled_sim/).

```bash
bash arduino/scripts/upload_rp2040_sketch.sh encoder_controller /dev/cu.usbmodem…
```
