# ESP32-P4 gauge firmware (Pi-less / vehicle displays)

Firmware for the three round MIPI displays: the **sensor_pack** UART (`arduino/sketches/sensor_pack/sensor_pack.ino`) feeds all three ESP32-P4 boards. Wire formats are in [`docs/protocols/`](../docs/protocols/).

## Themes and gestures (Classic + Fallout)

| Gesture | Scope | Effect |
|--------|--------|--------|
| Short touch release (center) | Center | Opens overlay menu (unchanged). |
| Hold center **≥ 10 s** | Global | Toggles **Classic** vs **Fallout** on all gauges; center **I2C-broadcasts** theme to speedo, TSPS, and optional ESP32-D; stored in **NVS** on each board. Release does not open the menu. |
| Hold center **≥ 3 s** (Classic only) | Center | Cycles sub-view: **Tach → Clock → Media**; NVS + I2C sync. |
| Hold TSPS **≥ 3 s** (Classic only) | TSPS | Toggles **classic_1 / classic_2** (NVS); short tap no longer switches layout. GPIO bench toggle may remain (see TSPS `platformio.ini`). |

**Fallout** uses fixed Pip-Boy–style layouts per gauge (no center sub-view cycling). **Speedo** follows global theme only (no sub-views).

## Cluster I2C (center master)

- **Bus**: second I2C peripheral on the center ESP32-P4 (`TwoWire(1)`), default **SDA = GPIO 47**, **SCL = GPIO 48**, **100 kHz**. Touch and BT metadata stay on **GPIO 7 / 8** (see [WIRING_PINOUT.md](../WIRING_PINOUT.md)).
- **Master**: center gauge. **Slaves**: speedo `0x3C`, TSPS `0x3D`, optional ESP32-D theme mirror `0x3E` (see `arduino/sketches/esp32_audio_bridge` — `Wire1` @ `0x3E` on pins you define, default 18/19 on the dev module).
- **Writes**: `[register][value]` per transaction (same as SMBus byte write).

| Register | Meaning |
|----------|---------|
| `0x01` | Theme: `0` = Classic, `1` = Fallout |
| `0x02` | Center sub-view: `0` Tach, `1` Clock, `2` Media (ignored on speedo/TSPS for layout; may still be stored) |
| `0x03` | TSPS classic variant: `0` classic_1, `1` classic_2 (TSPS local; not broadcast from center) |

Shared constants live in `esp32_p4_common/include/cluster_i2c.h`.

## Layout

| Directory | Role |
|-----------|------|
| `esp32_p4_common/` | Shared **library**: `SensorLink` / `sensor_packet` (UART parse, staleness, "NO DATA" stamp), MIPI `display_setup`, `cluster_i2c` (master + slave helpers), `elc_watchdog.h`, `el_camino_speed.h` (wheel RPM → MPH). Host unit tests: `test/host/run_tests.sh`. |
| `esp32_p4_ui/` | Shared **touch + modal overlays**: `p4_touch` (GT911 @ 0x5D/0x14 or CST816 @ 0x15 on SDA=7/SCL=8), `P4OverlayMenu`. |
| `esp32_p4_speedo/` | **LEFT**: speedometer (MPH from Hall `speed_rpm` + wheel constants). |
| `esp32_p4_center/` | **MIDDLE**: Classic tach / clock / media + Fallout screen; long-press gestures; I2C cluster master. |
| `esp32_p4_tsps/` | **RIGHT**: TSPS cluster; Classic layouts + Fallout; **3 s hold** for classic_1 vs classic_2 when theme is Classic. |
| `esp32_classic_bt_audio/` | PlatformIO wrapper; builds `arduino/sketches/esp32_audio_bridge` (A2DP + I2C metadata `0x2A` + optional cluster theme `0x3E` on `Wire1`). |
| `esp32_p4_display_litmus/` | Panel bring-up (solid color / HELLO). |

## Panel targets

Each app has **two** PlatformIO environments:

- **`*-waveshare3c4`** — Waveshare **3.4\"** 800×800 (SH8601), `esp32p4_es`, QIO.
- **`*-waveshare4c`** — **4\"** 720×720 (JD9365), PMIC + `TSPS_USE_JD9365`.

Use the env that matches the hardware; **do not** flash a 3.4C profile on a 4\" panel or vice versa.

## Build / flash

From the firmware directory:

```bash
cd firmware/esp32_p4_tsps   # or speedo / center
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
./flash.sh -e esp32-p4-tsps-waveshare3c4 --upload-port /dev/cu.usbmodem…
```

See each `platformio.ini` for env names and asset regeneration commands.

## UART wiring

**sensor_pack TX** → **all three ESP32 UART RX** in parallel (same 115200 8N1 stream). Only the **center** sends the 0xA5 keepalive back (its GPIO 4 → sensor RX); speedo and TSPS never transmit. See [WIRING_PINOUT.md](../WIRING_PINOUT.md) for the full vehicle harness.

## Center display: LVGL screen layer

The center app hands the panel to LVGL (`esp32_p4_ui/p4_screen.*`). LVGL renders in direct mode into the DSI
framebuffer and writes back only the rows it touched. The existing Arduino_GFX gauge and menu code draws into an
off-screen "legacy layer" canvas, which is the bottom LVGL object; new LVGL UI goes on `p4_screen_ui_parent()`.
The gauge renderers only run when something they show changes (`gauge_signature()` in `main.cpp`), and the layer is
compared band by band so LVGL redraws only what differs.

Console on the flashing USB port (115200, `Serial` = UART0): `help`, `perf` (fps and timing), `shot` (screenshot),
`tap <x> <y> [ms]` (simulated touch), plus the Wi-Fi/TZ commands. Grab a screenshot from the Mac with:

```bash
python3 tools/screenshot.py --port /dev/cu.usbmodem… -o center.png
```

The tool never toggles DTR/RTS: on these boards the CH343 auto-reset circuit reboots the ESP32 when one changes.

## Runtime behavior

- **Stale data:** no frame for 400 ms → gauges dim at rest with a red **NO DATA** stamp; odometer shows dashes.
- **Watchdog:** each display reboots if `loop()` stalls for 8 s. Anything slow (HTTPS lookups, Wi-Fi joins) runs asynchronously on the center.
- **Theme sync:** the center rebroadcasts theme/sub-view every 5 s.

## Fallout asset export (optional bitmaps)

Procedural Fallout UIs need no PNGs. To add sprites, place PNGs under `assets/gauges/fallout/` and run `scripts/export_fallout_rgb565_assets.py` (see `assets/gauges/fallout/README.md`).
