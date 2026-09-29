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
 phone ── A2DP/AVRCP ──► ESP32-D audio bridge (DigiDash slot 0) ── CAN ─────────── center  │
                                               center ── cluster I2C (theme) ──► speedo 0x3C, TSPS 0x3D
 encoder controller (RP2040: knobs, IR, SSD1322 OLED) ── UART ──► (not yet connected to an ESP32 — see Status)
```

**CAN bus** (DigiDash board, 500 kbit/s, one MCP2562 per node, 120 Ω at the left and right screens):

```
 left (speedo) ── slot 0 (audio bridge) ── slot 1 ── slot 2 ── center ── right (TSPS)
                                                                     └─ J17 short cable ── console (OLED + knobs)
```

The center re-sends each sensor frame and the theme on CAN; the three ESP32-DevKitC slots are drop-in accessory
sockets that find their slot number from board straps. The audio bridge talks to the center over CAN only (its I2C
link is for bench setups). The console (OLED + knobs) is its own board on a short 4-pin cable to J17 (+5 V, GND, CANL,
CANH); it is an unterminated branch, so the bus works with it unplugged. See `docs/protocols/can_bus.md`.

- **sensor_pack** is the source of truth for all readings and the odometer. One TX fans out to all three displays; only the center sends the 0xA5 keepalive back.
- **center** also owns GPS time/location, Wi-Fi (NTP, street name, time zone), touch menu, BLE, SD settings, and theme sync to the other two displays.
- If sensor data stops arriving for 400 ms, the gauges dim and show **NO DATA** instead of reading zero.

## Repository

| Path | What |
| --- | --- |
| `firmware/esp32_p4_display` | The round displays: one firmware, position (left / center / right) from a strap on the DigiDash board (PlatformIO, Arduino framework) |
| `firmware/elc_can` | Shared CAN (TWAI) library for the displays and slot cards: IDs, frame codecs, heartbeat. Host tests in `test/host/` |
| `firmware/esp32_p4_common` | Shared library: sensor UART (`SensorLink`), display setup, cluster I2C, watchdog, boot splash. Host tests in `test/host/` |
| `firmware/esp32_p4_ui` | Shared LVGL layer: gauge faces, the menu (on any round screen), media over CAN, touch |
| `firmware/esp32_classic_bt_audio` | PlatformIO wrapper that builds `arduino/sketches/esp32_audio_bridge` |
| `firmware/esp32_p4_display_litmus` | Panel bring-up test |
| `arduino/sketches/sensor_pack` | ATmega328P sensor + odometer firmware |
| `arduino/sketches/encoder_controller` | RP2040 knobs / IR / OLED |
| `arduino/scripts` | Flash helpers and `monitor_sensor_pack.py` bench decoder |
| `scripts/export_*_rgb565_assets.py` | Regenerate the `*_gen.h` image headers from `assets/` |
| `docs/protocols` | Wire formats: sensor UART, CAN bus, cluster/audio I2C, encoder UART |
| `tools/screenshot.py` | Screenshot a display over its USB console (`shot` command) |
| `WIRING_PINOUT.md` | Vehicle harness |

## Build and flash

Display boards: the same image on all three; each board finds its position from the strap on the DigiDash board
(pick the env that matches the panel: `display-waveshare3c4` = 3.4" 800×800, `display-waveshare4c` = 4" 720×720):

```bash
cd firmware/esp32_p4_display
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
./flash.sh -e display-waveshare3c4 --upload-port /dev/cu.usbmodem…
```

Or build only: `pio run -e display-waveshare3c4`. Platform and library versions are pinned in each `platformio.ini`.

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
firmware/elc_can/test/host/run_tests.sh           # CAN frame codecs
```

## Status / open items

- **Encoder controller is not wired into the ESP32 firmware yet.** Its UART protocol was designed for the Pi (`docs/protocols/nano_uart.md`). The plan is a console board (ESP32-S3, SSD1322 OLED on SPI, two encoders, two keys) on a 4-pin cable at the end of the CAN bus; its connector J17 is on the DigiDash board (`docs/protocols/can_bus.md`). The next board revision replaces the three DevKit slots with generic accessory ports and carrier boards (`docs/hardware/accessory_ports.md`).
- **D11 on the sensor pack** selects demo-sweep mode when HIGH and is a plain input with no pull resistor. Make sure the harness ties it LOW (or add a pull-down), or the gauges can show fake data.
- **Sensor data link integrity** is a 1-byte XOR. A CRC-8 plus a version byte would be stronger, but that means reflashing all four boards at once.
