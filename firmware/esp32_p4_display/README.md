# esp32_p4_display: the round screens' firmware

One firmware image for all three round screens. At boot each board reads its position strap and runs as the **left**
screen (speedometer, CAN node `0x02`), the **center** (tach, clock, media, efficiency; GPS, SD card, BLE, rear camera,
the cluster settings, the alert manager; CAN node `0x01`, cluster I2C master) or the **right** screen (fuel, oil,
coolant, charge; CAN node `0x03`). PlatformIO environments differ by panel only, never by position.

Users' view (themes, gestures, the menu, the console commands, panel targets, runtime behaviour):
[`../README.md`](../README.md). This file is the code map and the reference for building, configuring and extending.

```bash
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt   # or let flash.sh do it
./flash.sh -e display-waveshare3c4 --upload-port /dev/cu.usbmodem…    # build + flash (3.4" 800×800 panel)
```

## Status

- The root [`README.md`](../../README.md#status): the screens run on Waveshare P4 boards on the bench in all three
  positions, with the themes, menu, media, phone, call card, accessory pages and the camera; the camera capture still
  stalls about once a second on the bench. Nothing is in the car.
- `display-waveshare4c` (4", rev 3 silicon): `platformio.ini` keeps the illumination and reverse inputs off (their
  default pins are the board's USB D- / D+) "until the DigiDash wiring for this board is settled".
- Not implemented (from the code and `center_alerts.h`): the alert banner and list on the menu screen (alerts are
  acknowledged with `alerts ack` on the center's console); reading the GPS card's CAN frames `0x222`–`0x226` (the
  center takes GPS only from its UART; see *Known issues*).
- Firmware updates over CAN (`fw_overlay`, `elc_fw_esp32`): built and compile-checked; the transfer and the target
  are host-tested in `elc_can`. Not yet tried on a board: the PSRAM staging, the flash write and the rollback.
- No host tests in this directory. The shared libraries have theirs ([`elc_can`](../elc_can/),
  [`esp32_p4_common`](../esp32_p4_common/)), and the faces render on a Mac with `tools/gauge_preview`.

## Hardware it targets

| Env | Board (`../boards/` or PlatformIO) | Panel | Notes |
| --- | --- | --- | --- |
| `display-waveshare3c4` | `esp32-p4-waveshare-wifi6-touch-lcd-3c4` | Waveshare 3.4" 800×800 SH8601 | `esp32p4_es` chip variant, QIO, 360 MHz, DPI 48 MHz, backlight GPIO 26 PWM active low, `Serial` = UART0 (the CH343 USB port), `variants/esp32p4/pins_arduino.h` for the TF slot |
| `display-waveshare4c` | `esp32-p4-evboard` | Waveshare 4" 720×720 JD9365 | rev 3 silicon, 400 MHz, `Serial` = USB-Serial-JTAG (HWCDC), illumination and reverse inputs off |
| `display-ev720` | `esp32-p4-evboard` | EV board + 720×720 JD9165 module | LCD reset GPIO 27 |
| `display-ev7` | `esp32-p4-evboard` | EV board's stock 7" 1024×600 | faces in the centred 600×600 |

Never flash one panel's env onto the other panel. On the DigiDash board the screens plug into J5 (left), J1 (center)
and J4 (right); the header pinout is in [`WIRING_PINOUT.md`](../../WIRING_PINOUT.md).

### Pins (from `platformio.ini` and the sources)

| Function | GPIO | Who |
| --- | --- | --- |
| Position strap (header pin 17) | 4 (`DISPLAY_ROLE_STRAP_GPIO`) | all, read once at boot |
| Sensor UART RX | 5 (`SENSOR_UART_RX`) | all |
| Sensor UART TX (`0xA5` keepalive) | 4 (`SENSOR_UART_TX`) | center only; the side screens open the UART receive-only |
| CAN TX / RX (MCP2562) | 34 / 36 (`ELC_CAN_TX_PIN` / `ELC_CAN_RX_PIN`) | all |
| Touch and PMIC I2C | SDA 7 / SCL 8 | all |
| Cluster I2C (`TwoWire(1)`) | SDA 47 / SCL 48 | center master; left `0x3C`, right `0x3D` |
| GPS UART2 (DigiDash2 J6) | RX 3 / TX 2, 9600 (`GPS_UART_RX` / `_TX` / `_BAUD`) | center |
| Illumination input | 24 (`ELC_ILLUMINATION_GPIO`, low = night) | every screen built with it; a screen built with `-1` (the 4C env) dims from `0x210` flag bit3 instead. `can_bus.md` says the side screens have no input of their own on DigiDash2 |
| Reverse input | 25 (`ELC_REVERSE_GPIO`, active low) | center |
| Camera (TVP5150, BT.656) | D0–D7 = 20, 21, 22, 29, 30, 31, 32, 28; PCLK 49; VSYNC 50; HSYNC 51; RESETB 52; I2C `0x5C` or `0x5D` on the cluster bus | center (`center_camera.cpp`) |
| TF card (SDIO, 4-bit) | CLK 43, CMD 44, D0–D3 39–42 | center (`center_sd_settings.cpp`, `variants/esp32p4/pins_arduino.h`) |
| Right-screen bench layout button | 6 (`TSPS_THEME_GPIO`, pull-up) | right |
| Backlight | 26 (`TSPS_BACKLIGHT_PIN`) | all |

### Position strap

`display_role.cpp` reads GPIO4 with the internal pull-up and then the pull-down: low both times = left (J5: 1 kΩ to
GND), high both times = right (J4: 1 kΩ to 3V3), following the pull = center (J1: open). A board off the DigiDash
board reads open and runs as the center. `role left|center|right` stores an override in NVS (namespace `display`,
key `role`) and restarts; `role auto` clears it; `role` shows what the strap reads. Every 5 s (center) or 10 s (side)
a screen warns on its console if it hears a heartbeat with its own node number.

## Layout

| File | Role |
| --- | --- |
| `src/main.cpp` | Reads the position (`display_role_resolve()`), starts the USB CAN link (`elc_usb_begin()`), runs `center_setup` / `center_loop` or `side_setup` / `side_loop`. 24 KB loop stack |
| `src/display_role.{h,cpp}` | The strap, the NVS override, the `role` command, position to CAN node |
| `src/display_main.h` | The two entry pairs |
| `src/side_main.cpp` | Left and right screens: sensor UART (receive only), gauges, touch, theme from CAN `0x210` or the cluster I2C bus, `cluster_peer`, `menu_can`, `phone_can`, `chan_screen`, the side console, the right screen's Classic layout |
| `src/side_theme_input.{h,cpp}` | Right screen: the Classic layout in NVS (`tsps` / `c2`) and the bench GPIO that toggles it |
| `src/center_main.cpp` | Center: setup order, the loop (touch and gestures, faces, clock, media, camera, the 5 s cluster rebroadcast, trip distance, frame pacing to ~60 fps) |
| `src/center_ui.{h,cpp}` | Center state: view, theme, interface / media screens, right layout, the 3 s / 10 s holds, the older full-screen pages (sensor log, GPS data) drawn with Arduino_GFX, `0x210` / `0x213` / `0x220` sending, setting requests from `0x212` |
| `src/center_views.h` | `CenterViewMode` (0 Tach, 1 Clock, 2 Media, 3 Efficiency; stored in NVS) |
| `src/center_menu.{h,cpp}` | The `UiMenuHost` for the shared menu (`esp32_p4_ui/ui_menu.h`) on the center; brightness (NVS `bright`, `bfollow`); the hold ring |
| `src/center_can.{h,cpp}` | The center's `ElcCan`: re-sends each sensor frame as `0x100`–`0x104`, sends `0x210` / `0x213` / `0x220`, queues `0x212` requests, takes sensor and time frames from a USB relay while its own sources are silent, feeds `menu_can`, `phone_can`, `media_can` |
| `src/center_chan.{h,cpp}` | The center on the channel protocol: the core sensors on SENSOR (11 sensors), SYSTEM TIME (10 s), VEHICLE (5 Hz), PREFS (5 s and on change) |
| `src/center_alerts.{h,cpp}` | The alert manager (`ElcAlertManager`) and the center's own alerts as node `0x01`; `alerts`, `alerts ack [all]` |
| `src/center_audio_link.{h,cpp}` | Now playing, transport and album art for the center's media face (over `media_can`) |
| `src/center_camera.{h,cpp}` | Rear camera: TVP5150 probe and reset, DVP capture of BT.656 fields, YUV to RGB565 on the other core, PPA scale to 800×800, direct framebuffer mode, guide lines, reverse input, the `cam` bench checks |
| `src/center_gps.{h,cpp}` | NMEA on UART2: RMC / ZDA set the clock (`settimeofday`), position fix from RMC / GGA, the raw lines for the GPS data page |
| `src/center_time.{h,cpp}` | Local zone from the POSIX string in NVS `tz` (default `GMT0`); time from the bus while GPS has no fix |
| `src/center_sd_settings.{h,cpp}` | TF card: imports `/elcamino/settings.json` into NVS for keys NVS does not have yet, and writes it back |
| `src/center_ble.{h,cpp}` | BLE GATT info line (GAP name `ElCamino`), through the board's ESP32-C6 co-processor |
| `src/center_shell.{h,cpp}` | The center's serial console |
| `src/center_i2c_host.{h,cpp}` | An I2C scan helper for the touch bus (not called anywhere at present) |
| `variants/esp32p4/pins_arduino.h` | Pin variant for the 3.4C env: SDMMC slot 1 pins, the C6's SDIO pins |
| `platformio.ini`, `flash.sh`, `requirements.txt` | Build envs, the flash script, PlatformIO for the local venv |

## Build, flash, monitor

`flash.sh` makes a local `.venv` (Python 3.10 or newer from `PATH`, because pioarduino needs it), installs
`requirements.txt` (`platformio>=6.1.18`, `littlefs-python`), checks the RISC-V toolchain, removes the cached
`el_camino_p4_common` copy from `.pio/libdeps` and runs `platformio run -t upload` with the arguments you give.

```bash
./flash.sh -e display-waveshare3c4 --upload-port /dev/cu.usbmodem…
./flash.sh --erase-first -e display-waveshare3c4 --upload-port /dev/cu.usbmodem…   # full erase, then upload
.venv/bin/python -m platformio run -e display-waveshare4c                          # build only
.venv/bin/python -m platformio device monitor -e display-waveshare3c4 --port /dev/cu.usbmodem…
```

From `firmware/`, [`../flash_display.sh`](../flash_display.sh) runs the same with `ENV` (default
`display-waveshare3c4`) and `UPLOAD_PORT`. Every build runs [`../scripts/pio_pre_build.py`](../scripts/pio_pre_build.py)
(toolchain path, a fresh copy of the three local libraries, the GFX patches); see [`../boards/README.md`](../boards/README.md).

Serial is 115200. `monitor_rts = 0` and `monitor_dtr = 0`: on these boards a DTR / RTS change resets the P4 (GPIO35
low at reset = download mode). `tools/screenshot.py` never toggles them either:

```bash
python3 tools/screenshot.py --port /dev/cu.usbmodem… -o center.png
```

**In the car, without USB:** every build also writes `.pio/build/<env>/<env>.elcfw` ([`../scripts/elc_fw.py`](../scripts/elc_fw.py)).
Upload that from the console's Wi-Fi update page, or send it with `tools/fw_update.py` from a USB-CAN adapter on the
bus. The 4" file updates the left and right screens together. A screen takes it only while the car stands still (the
center: also with the camera off), shows the progress over the face, and restarts into it. A new image that resets in
its first 20 s, or that has no CAN in that time, is rolled back. See [the top README](../../README.md#firmware-updates-without-usb).

On the bench, with no CAN transceivers, run `python3 tools/can_relay.py` and the boards talk over their USB ports
([`docs/protocols/usb_link.md`](../../docs/protocols/usb_link.md)); it also supplies a simulated drive, the time and
the slot cards that are not built yet.

## Configuration

### Build flags (`platformio.ini`, `[common]`)

| Flag | Value | Meaning |
| --- | --- | --- |
| `SENSOR_UART_RX` / `SENSOR_UART_TX` | 5 / 4 | sensor UART |
| `DISPLAY_ROLE_STRAP_GPIO` | 4 | position strap |
| `ELC_CAN_TX_PIN` / `ELC_CAN_RX_PIN` | 34 / 36 | CAN (`-1` = off) |
| `ELC_DSI_NUM_FBS` | 2 | double-buffered panel (needs the pre-build GFX patch) |
| `GPS_UART_RX` / `GPS_UART_TX` / `GPS_UART_BAUD` | 3 / 2 / 9600 | center GPS (`38400` if u-center changed the module) |
| `WHEEL_CIRCUMFERENCE_M` / `SPEED_PULSES_PER_REV` | 2.1 / 1.0 | speed from wheel RPM |

Per env: `TSPS_WAVESHARE_3C4`, `TSPS_USE_JD9365`, `TSPS_PANEL_1024X600`, `TSPS_DSI_DPI_HZ`, `TSPS_BACKLIGHT_*`,
`ELC_ILLUMINATION_GPIO`, `ELC_REVERSE_GPIO`, `TSPS_THEME_GPIO`, `ARDUINO_USB_MODE` / `ARDUINO_USB_CDC_ON_BOOT`
(panel flags: [`esp32_p4_common`](../esp32_p4_common/README.md#configuration-build-flags)). In the sources:
`CLUSTER_REBROADCAST_MS` (5000), `SIDE_TOUCH_ENABLE` (1), `SIDE_TOUCH_SDA` / `_SCL` (7 / 8),
`CENTER_GPS_DIAG_RECENT_LINES` (15).

### NVS

| Namespace | Key | What |
| --- | --- | --- |
| `display` | `role` | bench position override (absent = the strap decides) |
| `center` | `tz` | POSIX time zone (default `GMT0`) |
| `center` | `view`, `ctheme` | center view, theme |
| `center` | `iface`, `media`, `mshown` | interface screen, media screen, media shown on the media screen |
| `center` | `rlayout` | right screen's Classic layout (1 = fuel + lamps) |
| `center` | `bright`, `bfollow` | brightness 10–100, dims with the headlights |
| `cluster` | `th` | theme on a side screen (from CAN or I2C) |
| `tsps` | `c2` | right screen's last layout (1 = four gauges) |

The center copies `tz`, `view`, `ctheme`, `iface`, `media` and `rlayout` to `/elcamino/settings.json` on the TF card
and, at boot, imports a key from it only when NVS does not have that key.

### Serial console (115200)

The full list with what each command does is in [`../README.md`](../README.md); `help` prints it on the board.

- Center (`center_shell.cpp`): `help`, `perf`, `shot [legacy]`, `demo on|off`, `theme 0|1|2`, `view 0..3`,
  `tap <x> <y> [ms]`, `drag <x> <y1> <y2> [ms]`, `knob left|right|press|back`, `menu on left|center|right`,
  `media on left|center|right`, `cam [on|off|auto|pins|timing|pclk|vsync|bits]`, `audio`, `can`,
  `alerts [ack [all]]`, `tz [<posix>|<hours>]`, `chan [query]`.
- Left / right (`gauge_console.cpp` + `side_main.cpp`): `help`, `perf`, `shot`, `demo on|off`, `theme 0|1|2`,
  `variant 0|1` (right; 0 = four gauges, 1 = fuel + lamps, asked of the center), `can`, `chan [query]`.
- Every screen: `role [left|center|right|auto]`, `phone`, `phone demo …`, `menu`, `menu rotate <n>|press|back|open <page>`,
  `fw` (build, app slots, an update arriving or why the last failed), `fw good` (keep a new image now).

## How it talks to the rest of the system

| Link | What | Spec |
| --- | --- | --- |
| Sensor UART (115200 8N1) | sensor frames in on every screen; `0xA5` keepalive from the center | [`sensor_uart.md`](../../docs/protocols/sensor_uart.md) |
| CAN, center sends | `0x100`–`0x104` (each sensor frame not itself from the bus), `0x210` (on change and every 5 s), `0x213` (1 Hz and on reverse), `0x220` (1 Hz), heartbeat `0x701` | [`can_bus.md`](../../docs/protocols/can_bus.md) |
| CAN, every screen | `0x212` setting requests (side screens), `0x211` art requests (media screen), `0x215` phone commands, `0x216`–`0x219` menu, heartbeats `0x702` / `0x703` | [`can_bus.md`](../../docs/protocols/can_bus.md) |
| CAN channels | ANNOUNCE as a display; UI pages of every accessory; the center sends SENSOR (core sensors), SYSTEM, ALERT and collects every node's alerts, receives LOG | [`can_channels.md`](../../docs/protocols/can_channels.md) |
| Cluster I2C | theme and view from the center to `0x3C` / `0x3D`, every 5 s; the TVP5150 on the same bus | [`cluster_i2c.md`](../../docs/protocols/cluster_i2c.md) |
| USB | the console, the USB CAN link, `shot` | [`usb_link.md`](../../docs/protocols/usb_link.md) |
| Settings ownership | the center owns the cluster settings; side screens and the console ask with `0x212` and follow `0x210` | [`can_bus.md`](../../docs/protocols/can_bus.md#cluster-settings) |

## Extending it

- **A center setting:** add the `ElcCanSetting` number in `elc_can_codec.h` and `can_bus.md`, apply it where the
  center takes `0x212` (`center_ui.cpp`), store it in the `center` namespace, add it to `center_sd_settings.cpp` if it
  should survive a new board, and send it in `0x210` if the side screens need it.
- **A center view:** add a `CenterViewMode` (keep the stored numbers), a `GaugeFace` and faces in all three themes in
  [`esp32_p4_ui`](../esp32_p4_ui/README.md#extending-it), map it in `face_for()` (`center_main.cpp`), and update the
  view cycle and the menu.
- **A console command:** center: a branch in `handle_shell_line()` (`center_shell.cpp`) and the `help` text; side:
  `s_console.on_line` in `side_main.cpp`. Update [`../README.md`](../README.md).
- **A new center alert:** append to the `Own` list in `center_alerts.cpp` (the numbers are stable) and to the *Center
  alerts* table in `can_channels.md`.
- **Anything slow** (I/O that can block) goes in its own task: the loop watchdog reboots a screen whose `loop()` stalls
  for 8 s.

## Known issues and open items

- Camera capture stalls about once a second on the bench (root `README.md`). The `cam timing`, `cam pins` and
  `cam bits` commands exist to chase it.
- `platformio.ini` says GPS "normally arrives over CAN from the GPS card", but nothing here decodes the GPS card's
  frames `0x222`–`0x226`: the center reads NMEA from UART2 only, and takes `0x220` time from a USB relay.
- The sensor log and GPS data pages are still the older Arduino_GFX full-screen pages on the center.
- `center_shell.cpp`'s `help` says `menu … open <0..6>`; page 7 (Accessories) also opens.
- Stale comments: `center_main.cpp` ("fresh I2C data" before the paint; the bridge is on CAN only).
- [`../flash_display.sh`](../flash_display.sh) defaults to one bench board's USB serial number as the port; pass
  `UPLOAD_PORT`.
