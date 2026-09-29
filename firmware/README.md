# ESP32-P4 gauge firmware (Pi-less / vehicle displays)

Firmware for the three round MIPI displays: the **sensor_pack** UART (`arduino/sketches/sensor_pack/sensor_pack.ino`) feeds all three ESP32-P4 boards. Wire formats are in [`docs/protocols/`](../docs/protocols/).

## Themes and gestures (Classic, Modern, Fallout)

| Gesture | Scope | Effect |
|--------|--------|--------|
| Short touch release (center) | Center | Opens the menu. |
| Hold center **≥ 10 s** | Global | Next theme (**Classic → Modern → Fallout**) on all gauges; center **I2C-broadcasts** theme to speedo, TSPS, and optional ESP32-D; stored in **NVS** on each board. Release does not open the menu. |
| Hold center **≥ 3 s** | Center | Next view: **Tach → Clock → Media → Efficiency**; NVS + I2C sync. |
| Hold TSPS **≥ 3 s** (Classic only) | TSPS | Toggles **classic_1 / classic_2** (NVS). GPIO bench toggle may remain (see TSPS `platformio.ini`). |

Every face exists in all three themes. **Speedo** follows the global theme only (no sub-views).

## Gauge faces (vector, LVGL)

All faces are drawn by `esp32_p4_ui/src/gauges/` (API: `elc_gauges.h`); there are no bitmap assets. The static
dial of a face (scale, numerals, chrome rings, spun-aluminium caps, gradients) is painted once in software with
anti-aliased shapes when the face is shown (0.3–0.8 s on the P4), then cached; needles, arcs, lamps and readouts are
live LVGL objects that repaint only the pixels that change. Faces are designed on an 800-unit square and scale to
the panel (720×720, 800×800, or the centred 600×600 of a 1024×600 panel).

- `faces_classic.cpp`: black glass, cream numerals, chrome, red needles (layouts follow the original Classic art in
  `assets/gauges/`).
- `faces_modern.cpp`: flat dark glass with an orange progress arc (the cluster prototype's Modern theme).
- `faces_fallout.cpp`: green phosphor terminal with segment bars and scanlines.

The faces render on a Mac too, with the same code, which is how layouts are iterated without flashing:

```bash
tools/gauge_preview/build.sh                                  # needs one PlatformIO build of the center (for LVGL)
python3 tools/gauge_preview/preview.py faces.png 0,1,0 2,1,0 1,1,0   # theme,face,variant — see the script
```

## Cluster I2C (center master)

- **Bus**: second I2C peripheral on the center ESP32-P4 (`TwoWire(1)`), default **SDA = GPIO 47**, **SCL = GPIO 48**, **100 kHz**. Touch and BT metadata stay on **GPIO 7 / 8** (see [WIRING_PINOUT.md](../WIRING_PINOUT.md)).
- **Master**: center gauge. **Slaves**: speedo `0x3C`, TSPS `0x3D`, optional ESP32-D theme mirror `0x3E` (see `arduino/sketches/esp32_audio_bridge` — `Wire1` @ `0x3E` on pins you define, default 18/19 on the dev module).
- **Writes**: `[register][value]` per transaction (same as SMBus byte write).

| Register | Meaning |
|----------|---------|
| `0x01` | Theme: `0` = Classic, `1` = Fallout, `2` = Modern (unknown values fall back to Classic) |
| `0x02` | Center sub-view: `0` Tach, `1` Clock, `2` Media, `3` Efficiency (ignored on speedo/TSPS) |
| `0x03` | TSPS classic variant: `0` classic_1, `1` classic_2 (TSPS local; not broadcast from center) |

Shared constants live in `esp32_p4_common/include/cluster_i2c.h`.

## CAN bus

On the DigiDash board every display and accessory slot also sits on a 500 kbit/s CAN bus (one MCP2562 each; displays
TX GPIO34 / RX GPIO36, slots TX GPIO32 / RX GPIO33). The center re-sends sensor frames (`0x100`–`0x104`) and the
theme (`0x210`); speedo/TSPS take the theme from CAN or I2C, whichever arrives. The audio bridge sends now-playing,
transport and album art in its slot range, and in the car that is the only audio link: the center's I2C audio code
(`0x2A`, theme to `0x3E`) is compiled in only by its `*-bench` envs (`CENTER_AUDIO_I2C=1`), paired with the bridge's
`esp32dev_bench` env. Every node sends a 1 Hz heartbeat. Pins are build flags
(`ELC_CAN_TX_PIN` / `ELC_CAN_RX_PIN`, `-1` = off). Spec: [docs/protocols/can_bus.md](../docs/protocols/can_bus.md).

## Layout

| Directory | Role |
|-----------|------|
| `esp32_p4_common/` | Shared **library**: `SensorLink` / `sensor_packet` (UART parse, staleness, demo drive), MIPI `display_setup`, `cluster_i2c` (master + slave helpers), `elc_watchdog.h`, `el_camino_speed.h` (wheel RPM → MPH). Host unit tests: `test/host/run_tests.sh`. |
| `elc_can/` | Shared **CAN** library (displays and slot cards): IDs, frame codecs, TWAI driver wrapper with heartbeat and bus-off recovery. Host unit tests: `test/host/run_tests.sh`. |
| `esp32_p4_ui/` | Shared **screen, touch and faces**: `p4_screen` (LVGL, double-buffered panel, optional legacy GFX layer, screenshot), `gauges/` (all gauge faces), `fonts/` (LVGL fonts), `gauge_console` (bench console for speedo/TSPS), `p4_touch` (GT911 @ 0x5D/0x14 or CST816 @ 0x15 on SDA=7/SCL=8), LVGL config. |
| `esp32_p4_speedo/` | **LEFT**: speedometer (MPH from Hall `speed_rpm` + wheel constants). |
| `esp32_p4_center/` | **MIDDLE**: tach / clock / media / efficiency faces, LVGL menu, long-press gestures; I2C cluster master. |
| `esp32_p4_tsps/` | **RIGHT**: fuel / oil / coolant / charge; Classic has two layouts (**3 s hold** switches). |
| `esp32_classic_bt_audio/` | PlatformIO wrapper; builds `arduino/sketches/esp32_audio_bridge` (A2DP + CAN accessory in DigiDash slot 0: now-playing, transport, album art). `esp32dev_bench` adds the bench I2C slaves (`0x2A` metadata, `0x3E` theme). |
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

See each `platformio.ini` for env names.

## UART wiring

**sensor_pack TX** → **all three ESP32 UART RX** in parallel (same 115200 8N1 stream). Only the **center** sends the 0xA5 keepalive back (its GPIO 4 → sensor RX); speedo and TSPS never transmit. See [WIRING_PINOUT.md](../WIRING_PINOUT.md) for the full vehicle harness.

## LVGL screen layer

All three apps hand the panel to LVGL (`esp32_p4_ui/p4_screen.*`). LVGL renders strips in on-chip RAM into the
hidden one of two DSI framebuffers, the panel flips on each refresh, and the redrawn rows are copied into the other
buffer by DMA, so nothing tears. Layers, bottom to top: the optional legacy Arduino_GFX canvas (center only, for
its older full-screen pages), the gauge faces (`p4_screen_gauge_parent()`), and UI (`p4_screen_ui_parent()`).

The center menu (`center_menu.cpp`) is pure LVGL: a wheel on the home page, and Wi-Fi, Brightness, Theme and
Service pages, rebuilt in the active theme's colors and fonts each time it opens. The Wi-Fi password keyboard,
sensor log, GPS data and Phone pages are still the older Arduino_GFX full-screen pages.

Console on the flashing USB port (115200, `Serial` = UART0 on the 3.4C boards):

- center: `help`, `perf`, `shot [legacy]`, `demo on|off`, `theme 0|1|2`, `view 0..3`, `tap <x> <y> [ms]`,
  `drag <x> <y1> <y2> <ms>`, `knob left|right|press`, `can`, plus the Wi-Fi/TZ commands;
- speedo / TSPS: `help`, `perf`, `shot`, `demo on|off`, `theme 0|1|2`, `variant 0|1` (TSPS), `can`.

Grab a screenshot from the Mac with:

```bash
python3 tools/screenshot.py --port /dev/cu.usbmodem… -o center.png
```

The tool never toggles DTR/RTS: on these boards the CH343 auto-reset circuit reboots the ESP32 when one changes.

## Runtime behavior

- **Stale data:** no frame for 400 ms → needles fall to rest, the face dims and shows **NO DATA**; odometer shows dashes.
- **Watchdog:** each display reboots if `loop()` stalls for 8 s. Anything slow (HTTPS lookups, Wi-Fi joins) runs asynchronously on the center.
- **Theme sync:** the center rebroadcasts theme/sub-view every 5 s.
