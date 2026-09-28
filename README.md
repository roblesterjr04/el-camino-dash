# El Camino Dash

Digital gauge cluster for a 1964 El Camino: three round MIPI-DSI displays driven by ESP32-P4 boards, fed by an ATmega328P sensor pack, with a classic-ESP32 Bluetooth audio bridge.

The earlier Raspberry Pi / pygame dashboard is shelved; it is preserved at git tag `pi-pygame-final`.

## System

```
 vehicle senders ──► sensor_pack (ATmega328P) ── UART 115200, 36-byte frames ──┬──► speedo  (ESP32-P4, left)
 (fuel, temp, oil,      │  FRAM (MB85RC @0x50):                                 ├──► center  (ESP32-P4, middle)
  hall, coil, IGN, ALT) │  odometer, A/B slots                                  └──► TSPS    (ESP32-P4, right)
                                                                                        ▲  ▲
 GPS (NEO-6M, NMEA 9600) ─────────────────────────────────── UART2 ─────────────── center │
 phone ── A2DP/AVRCP ──► ESP32-D audio bridge ── I2C 0x2A (display bus) ───────── center  │
                                               center ── cluster I2C (theme) ──► speedo 0x3C, TSPS 0x3D, ESP32-D 0x3E
 encoder controller (RP2040: knobs, IR, SSD1322 OLED) ── UART ──► (not yet connected to an ESP32 — see Status)
```

- **sensor_pack** is the source of truth for all readings and the odometer. One TX fans out to all three displays; only the center sends the 0xA5 keepalive back.
- **center** also owns GPS time/location, Wi-Fi (NTP, street name, time zone), touch menu, BLE, SD settings, and theme sync to the other two displays.
- If sensor data stops arriving for 400 ms, the gauges dim and show **NO DATA** instead of reading zero.

## Repository

| Path | What |
| --- | --- |
| `firmware/esp32_p4_speedo`, `_center`, `_tsps` | Display apps (PlatformIO, Arduino framework) |
| `firmware/esp32_p4_common` | Shared library: sensor UART (`SensorLink`), display setup, cluster I2C, watchdog, boot splash. Host tests in `test/host/` |
| `firmware/esp32_p4_ui` | Touch + overlay menu |
| `firmware/esp32_classic_bt_audio` | PlatformIO wrapper that builds `arduino/sketches/esp32_audio_bridge` |
| `firmware/esp32_p4_display_litmus` | Panel bring-up test |
| `arduino/sketches/sensor_pack` | ATmega328P sensor + odometer firmware |
| `arduino/sketches/encoder_controller` | RP2040 knobs / IR / OLED |
| `arduino/scripts` | Flash helpers and `monitor_sensor_pack.py` bench decoder |
| `scripts/export_*_rgb565_assets.py` | Regenerate the `*_gen.h` image headers from `assets/` |
| `docs/protocols` | Wire formats: sensor UART, cluster/audio I2C, encoder UART |
| `tools/screenshot.py` | Screenshot a display over its USB console (`shot` command) |
| `WIRING_PINOUT.md` | Vehicle harness |

## Build and flash

Display boards (pick the env that matches the panel: `-waveshare3c4` = 3.4" 800×800, `-waveshare4c` = 4" 720×720):

```bash
cd firmware/esp32_p4_center
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
./flash.sh -e esp32-p4-center-waveshare3c4 --upload-port /dev/cu.usbmodem…
```

Or build only: `pio run -e esp32-p4-center-waveshare3c4`. Platform and library versions are pinned in each `platformio.ini`.

Sensor pack (ATmega328P):

```bash
bash arduino/scripts/install_arduino_cli.sh
bash arduino/scripts/upload_sensor_pack.sh /dev/cu.usbmodem101 arduino:avr:uno
python3 arduino/scripts/monitor_sensor_pack.py --port /dev/cu.usbmodem101
```

BT audio bridge (classic ESP32 only): `cd firmware/esp32_classic_bt_audio && pio run -e esp32dev -t upload`.

**The sensor pack and the three displays must be flashed together** when the wire format changes (see `docs/protocols/sensor_uart.md`).

## Tests

```bash
firmware/esp32_p4_common/test/host/run_tests.sh   # sensor UART parser, runs on the host with ASan/UBSan
```

## Status / open items

- **Encoder controller is not wired into the ESP32 firmware yet.** Its UART protocol was designed for the Pi (`docs/protocols/nano_uart.md`); it needs a host (likely a spare UART on the center) and a decision on what the knobs and OLED do.
- **D11 on the sensor pack** selects demo-sweep mode when HIGH and is a plain input with no pull resistor. Make sure the harness ties it LOW (or add a pull-down), or the gauges can show fake data.
- **Sensor data link integrity** is a 1-byte XOR. A CRC-8 plus a version byte would be stronger, but that means reflashing all four boards at once.
