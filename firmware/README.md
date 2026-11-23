# ESP32-P4 gauge firmware (Pi-less / vehicle displays)

Firmware for the three round MIPI displays: the **sensor_pack** UART (`arduino/sketches/sensor_pack/sensor_pack.ino`) feeds all three ESP32-P4 boards. Wire formats are in [`docs/protocols/`](../docs/protocols/).

## Themes and gestures (Classic, Modern, Fallout)

| Gesture | Scope | Effect |
|--------|--------|--------|
| Short touch release | Interface screen | Opens the menu on whichever round screen is the **interface screen** (Menu → Screens; center by default). A tap elsewhere does nothing. |
| Hold center **≥ 10 s** | Global | Next theme (**Classic → Modern → Fallout**) on all gauges; center **I2C-broadcasts** theme to speedo, TSPS, and optional ESP32-D; stored in **NVS** on each board. Release does not open the menu. |
| Hold center **≥ 3 s** | Center | Next view: **Tach → Clock → Media → Efficiency** (Media only while the center is the media screen); NVS + CAN sync. |
| Hold speedo / TSPS **≥ 3 s** on the **media screen** | That screen | Toggles its media view (the gauge stays the default). |
| Hold TSPS **≥ 3 s** (Classic only, not the media screen) | TSPS | Toggles **classic_1 / classic_2**. The center owns this setting (CAN setting 11, NVS `rlayout` + `settings.json`, `0x210` flag bit2 = fuel + lamps), so the hold (and the GPIO bench toggle, and `variant`) asks the center and the right screen follows its answer; with no center heard it switches locally. The right screen keeps the last layout in its own NVS for boot. |

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
tools/gauge_preview/build.sh                                  # needs one PlatformIO build of the display firmware (for LVGL)
python3 tools/gauge_preview/preview.py faces.png 0,1,0 2,1,0 1,1,0   # theme,face,variant — see the script
python3 tools/gauge_preview/preview.py call.png 0,3,0,0,incoming 2,3,0,0,active+muted 1,3,0,0,pair   # call card
python3 tools/gauge_preview/preview.py phone.png 0,1,0,0,idle,6 2,1,0,0,pair,6 1,1,0,0,discover,6 # menu Phone page
```

**Call card.** While a call exists or a pairing code waits, the media screen covers its face (and the menu, if open)
with a card in the theme's colours: the state (incoming, calling, call time, on hold, call waiting), the caller,
Muted / Audio on phone, or "Pair with …?" and the six-digit code, and at the bottom two keys that do what the
console's two soft keys do now (Decline / Answer, Ignore / Answer, End / Mute, End / Swap, End, Reject / Pair): a tap
sends the phone command (`0x215`) from this screen's node. Other touches on the card stop there. On the center it never
covers the reversing camera.
`esp32_p4_ui/src/phone_can.cpp`; protocol in `docs/protocols/can_bus.md` (Phone).

## Cluster I2C (center master)

- **Bus**: second I2C peripheral on the center ESP32-P4 (`TwoWire(1)`), default **SDA = GPIO 47**, **SCL = GPIO 48**, **100 kHz**. Touch stays on **GPIO 7 / 8** (see [WIRING_PINOUT.md](../WIRING_PINOUT.md)).
- **Master**: center gauge. **Slaves**: speedo `0x3C`, TSPS `0x3D`.
- **Writes**: `[register][value]` per transaction (same as SMBus byte write).

| Register | Meaning |
|----------|---------|
| `0x01` | Theme: `0` = Classic, `1` = Fallout, `2` = Modern (unknown values fall back to Classic) |
| `0x02` | Center sub-view: `0` Tach, `1` Clock, `2` Media, `3` Efficiency (ignored on speedo/TSPS) |
| `0x03` | TSPS classic variant: `0` classic_1, `1` classic_2 (TSPS local; not broadcast from center) |

Shared constants live in `esp32_p4_common/include/cluster_i2c.h`.

## CAN bus

On the DigiDash board every display and accessory carrier also sits on a 500 kbit/s CAN bus (one MCP2562 each; displays
TX GPIO34 / RX GPIO36, the audio carrier TX GPIO32 / RX GPIO33). The center re-sends sensor frames (`0x100`–`0x104`) and the
theme (`0x210`); speedo/TSPS take the theme from CAN or I2C, whichever arrives. The audio bridge sends now-playing,
transport and album art in its node's range; that is the only audio link. There are no bench builds: every board runs
the firmware that ships, and the bench reaches the bus through the USB CAN link (`tools/can_relay.py`,
[docs/protocols/usb_link.md](../docs/protocols/usb_link.md)) in place of the old I2C bench builds. Every node sends a 1 Hz heartbeat. Pins are build flags
(`ELC_CAN_TX_PIN` / `ELC_CAN_RX_PIN`, `-1` = off). Spec: [docs/protocols/can_bus.md](../docs/protocols/can_bus.md).

## Layout

| Directory | Role |
|-----------|------|
| `esp32_p4_common/` | Shared **library**: `SensorLink` / `sensor_packet` (UART parse, staleness, demo drive), MIPI `display_setup`, `cluster_i2c` (master + slave helpers), `elc_watchdog.h`, `el_camino_speed.h` (wheel RPM → MPH). Host unit tests: `test/host/run_tests.sh`. |
| `elc_can/` | Shared **CAN** library (displays and accessories): IDs, frame codecs, TWAI driver wrapper with heartbeat and bus-off recovery. Host unit tests: `test/host/run_tests.sh`. |
| `esp32_p4_ui/` | Shared **screen, touch and faces**: `p4_screen` (LVGL, double-buffered panel, optional legacy GFX layer, screenshot), `gauges/` (all gauge faces), `fonts/` (LVGL fonts), `ui_menu` + `ui_theme` (menu and overlay colours / fonts per theme), `media_can` / `phone_can` (audio bridge on CAN: now playing and art; phone state and the call card), `gauge_console` (bench console for speedo/TSPS), `p4_touch` (GT911 @ 0x5D/0x14 or CST816 @ 0x15 on SDA=7/SCL=8), LVGL config. |
| `esp32_p4_display/` | **All three round displays**, one image. At boot each board reads its position strap (header pin 17 = GPIO4 on the DigiDash board: J5 left → 1 kΩ to GND, J4 right → 1 kΩ to its 3V3, J1 center → open) and runs as **LEFT** speedometer, **CENTER** (tach / clock / media / efficiency, GPS, SD, BLE, camera, the cluster settings; I2C cluster master) or **RIGHT** fuel / oil / coolant / charge (Classic has two layouts). `src/main.cpp` picks `center_*` or `side_*`; see `src/display_role.h`. |
| `esp32_classic_bt_audio/` | PlatformIO wrapper; builds `arduino/sketches/esp32_audio_bridge`: the audio sink (one 48 kHz output: Bluetooth, the radio on the I2S rail's RET, aux in; a 10-band equaliser, master volume, fader / balance; AUDIO_CMD and NET RAIL) and the Bluetooth bridge (A2DP + HFP; now-playing, transport, album art in its node's range). Node 0x14 + slot − 1 on the rev 2 audio card (`audiocarrier`; with no main board it reads no slot and takes 0x14), 0x10 + straps on a rev 1 carrier. `esp32wrover` (DevKitC-VE, PSRAM; the default) or `esp32dev` (WROOM DevKitC-32). Host tests: `test/host/run_tests.sh` (chimes, the DSP, the sink). |
| `sensor_card/` | **Sensor card** (`SensorCard/`, ATmega2560, CAN node 0x04; PlatformIO `atmelavr`): senders, speed, tach, ignition, battery; calibration and odometer in the FRAM; frames `0x100`–`0x104`, the SENSOR / UI / ALERT channels, the J8 serial fallback, the USB CAN link and a bench console on USART0. Pins are an Arduino Mega's, so it runs on a Mega with a CAN shield. Logic host-tested: `test/host/run_tests.sh`. |
| `sensor_card_stm32/` | **Sensor card rev B** (`SensorCard2/`, STM32G0B1KBU6, the same CAN node 0x04; PlatformIO `ststm32`, STM32duino core + ST HAL for the ADC scan and FDCAN1): rev A's logic built from `sensor_card/src`, only the hardware layer is new. Not yet run on a card. See its `README.md`. Host tests: `test/host/run_tests.sh` (rev A's suite plus its own). |
| `esp32s3_console/` | **Console board** (`ConsoleBoardRS/`, ESP32-S3, CAN node 0x13): the car's radio head unit. SSD1322 OLED and two RetroSound knobs (left Volume, right Select; each turns, pushes and has a ring that pushes left / right), no keys; the right ring is two soft keys labelled on the OLED. Its own OLED menu: Source, Radio, Phone (calls, pairing, paired phones), Accessories, Display, Trip A, Service. See its `README.md`. UI state machine host-tested: `test/host/run_tests.sh`. |
| `esp32_p4_display_litmus/` | Panel bring-up (solid color / HELLO). |

## Panel targets

One environment per panel, never per position (`esp32_p4_display/platformio.ini`):

- **`display-waveshare3c4`** — Waveshare **3.4\"** 800×800 (SH8601), `esp32p4_es`, QIO.
- **`display-waveshare4c`** — **4\"** 720×720 (JD9365), PMIC + `TSPS_USE_JD9365`.
- **`display-ev720`**, **`display-ev7`** — Espressif P4 EV board with a 720×720 JD9165 or its stock 7\" 1024×600.

Use the env that matches the hardware; **do not** flash a 3.4C profile on a 4\" panel or vice versa.

**Position.** The strap decides on the DigiDash board. Off the board (bench) the pin is open, so a board runs as the
center; `role left|center|right` on its serial console stores an override in NVS and restarts, `role auto` clears it,
and `role` shows what the strap reads. Two boards claiming the same position print a warning (their CAN heartbeats
collide).

## Build / flash

From the firmware directory:

```bash
cd firmware/esp32_p4_display
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
./flash.sh -e display-waveshare3c4 --upload-port /dev/cu.usbmodem…
```

Or `firmware/flash_display.sh` (same image for every screen; `ENV=display-waveshare4c` for the 4" panel).

## Firmware updates without USB

In the car the screens' USB ports are out of reach, so the screens and the console also take new firmware over CAN.
How to do it is in the [top README](../README.md#firmware-updates-without-usb); the wire format is
[the firmware profile](../docs/protocols/can_channels.md#the-firmware-profile-kind-2). In the code:

- Every build also writes `<env>.elcfw` next to `firmware.bin` (`scripts/elc_fw.py`, an `extra_scripts` entry: it
  also sets `ELC_FW_VARIANT` to the environment's name, which a board compares with the file's build). The product
  number is `custom_elc_product` in `platformio.ini`.
- `elc_can`: `elc_bulk` (BULK WRITE, both ends), `elc_fw` (the file header and `ElcFwTarget`), `elc_fw_esp32` (the
  spare OTA slot, PSRAM staging, keeping or rolling back a new image). A board calls `elc_fw_esp32_begin()` after its
  channels are up and `elc_fw_esp32_poll()` every loop.
- The screens (`side_main.cpp`, `center_can.cpp`) take updates only while the sensors say the car stands still (and,
  on the center, the camera is off), and cover the face with the progress (`esp32_p4_ui/src/fw_overlay.cpp`). A new
  image is kept after 20 s with CAN up; a reset before that brings the old one back.
- The console (`esp32s3_console/src/fw_portal.cpp`) runs the Wi-Fi page and sends the screens' files over CAN; it also
  takes its own updates over CAN from `tools/fw_update.py`.

## UART wiring

**sensor_pack TX** → **all three ESP32 UART RX** in parallel (same 115200 8N1 stream). Only the **center** sends the 0xA5 keepalive back (its GPIO 4 → sensor RX, bench harness only: J1 leaves that pin open); the left and right screens open the UART receive-only, because on the DigiDash board their GPIO 4 is the position strap. See [WIRING_PINOUT.md](../WIRING_PINOUT.md) for the full vehicle harness.

## LVGL screen layer

All three apps hand the panel to LVGL (`esp32_p4_ui/p4_screen.*`). LVGL renders strips in on-chip RAM into the
hidden one of two DSI framebuffers, the panel flips on each refresh, and the redrawn rows are copied into the other
buffer by DMA, so nothing tears. Layers, bottom to top: the optional legacy Arduino_GFX canvas (center only, for
its older full-screen pages), the gauge faces (`p4_screen_gauge_parent()`), and UI (`p4_screen_ui_parent()`).

The menu (`esp32_p4_ui/ui_menu.cpp`) is pure LVGL: a wheel on the home page, and Brightness, Theme, Screens,
Service, Phone and Accessories (every accessory's UI page, `accessory_can`) pages, rebuilt in the active theme's colors and fonts each time it opens. It runs on any round screen:
the center backs it with its own state (`center_menu.cpp`), the speedo and TSPS with CAN (`cluster_peer.cpp`: setting
requests to the center, state from `0x210` / `0x213`, see `docs/protocols/can_bus.md`). The Screens page
picks the **interface screen** (menu) and the **media screen**. The sensor log and GPS data pages are still the
older Arduino_GFX full-screen pages on the center; from a side screen they open there. The cluster never uses Wi-Fi
(the one exception is the console's firmware-update hotspot: [Firmware updates without USB](#firmware-updates-without-usb)).

**Phone page** (any screen, from `phone_can`): the connected phone with signal and battery; *Pair new phone* (opens
the bridge's pairing window, then "Discoverable as ElCamino-Media · m:ss left" and *Stop*); the paired list (choose
one: *Connect* / *Disconnect*, *Forget* with a confirm); *Voice assistant* and *Redial* while a hands-free phone is
connected. A phone asking to pair takes over the page ("Pair with …?", the code, *Pair* / *Reject*) and opens it on
the interface screen by itself (not over the reversing camera); while the page is open there, the call card leaves
the pairing request to it. Commands go to the bridge on `0x215` from this board's node.

**Remote menu input** (`menu_can`, every screen): the interface screen applies `0x216` from any node (turn moves
the wheel, brightness or the Phone page's focus; push opens / presses; Back goes back a page, then closes; Open opens
a page) and sends `0x217` + texts `0x218` / `0x219` (`ui_menu_describe()`: the focused item or the page's key fact, and its
value or hint) on change, every second while open and once on close. The console has its own OLED menu and does
not send `0x216`; `menu …` on a screen's serial console (below) does the same locally. The center also sends the time (`0x220`, 1 Hz:
UTC from GPS, local offset from the `tz` setting, GPS fix flag), and sets `0x213` bit7 while reversing (camera shown or reverse input on).

Console on the flashing USB port (115200, `Serial` = UART0 on the 3.4C boards):

- center: `help`, `perf`, `shot [legacy]`, `demo on|off`, `theme 0|1|2`, `view 0..3`, `tap <x> <y> [ms]`,
  `drag <x> <y1> <y2> <ms>`, `knob left|right|press|back`, `menu on left|center|right`, `media on left|center|right`,
  `cam [on|off|auto]` (rear camera view; `auto` follows the reverse input), `cam pins` / `cam timing` / `cam pclk` /
  `cam vsync` (capture wiring checks), `cam bits` (with the view on: per data line D0–D7, the share of ones and
  changes, valid BT.656 timing codes, and horizontal blanking compared bit by bit, naming stuck, noisy or swapped lines),
  `can`, `alerts` (every node's alerts; `alerts ack` acknowledges the top one, `alerts ack all` all of them),
  `tz [<posix>|<hours>]` (local zone: a POSIX string such as `CST6CDT,M3.2.0,M11.1.0` keeps DST without a
  network; a bare hour offset such as `-6` is fixed, no DST; `tz` alone shows it);
- left / right: `help`, `perf`, `shot`, `demo on|off`, `theme 0|1|2`, `variant 0|1` (right), `can`;
- every screen: `role [left|center|right|auto]` (position; see above), `phone` (phone state from the audio bridge,
  with the paired list and the pairing window) and `phone demo idle|discover|incoming|active [muted] [onphone]|waiting|held|pair|off`
  (synthetic bridge frames fed locally, to see the call card and the Phone page without a phone; the page's commands
  act on the demo; `off` hands back to the bridge); `menu` (the `0x217` state and texts this board would send) and
  `menu rotate <n>|press|back|open <0..7>` (as `0x216` from node `0x13`); `fw` (the firmware: build, app slots, an
  update arriving or the last one's result) and `fw good` (keep a new image now instead of after 20 s).

Grab a screenshot from the Mac with:

```bash
python3 tools/screenshot.py --port /dev/cu.usbmodem… -o center.png
```

The tool never toggles DTR/RTS: on these boards the CH343 auto-reset circuit reboots the ESP32 when one changes.

## Runtime behavior

- **Stale data:** no frame for 400 ms → needles fall to rest, the face dims and shows **NO DATA**; odometer shows dashes.
- **Watchdog:** each display reboots if `loop()` stalls for 8 s. Anything slow belongs in its own task.
- **Theme sync:** the center rebroadcasts theme/sub-view every 5 s.
