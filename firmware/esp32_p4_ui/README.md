# esp32_p4_ui: screen layer, gauge faces, menu and CAN UI modules

A PlatformIO library (`library.json`, name `el_camino_p4_ui`) shared by the three round screens: the LVGL screen
layer that owns the panel, every gauge face in every theme, the LVGL fonts, the touch driver, the cluster menu and the
CAN modules that feed the UI (media, phone and call card, the console's menu input, the side screens' view of the
center, the channel protocol on a screen). The application that uses it is
[`../esp32_p4_display`](../esp32_p4_display/); the panel, the sensor link and the I2C bus are in
[`../esp32_p4_common`](../esp32_p4_common/). Themes, gestures, the menu pages and the console commands are described
for users in [`../README.md`](../README.md); this file is the code map.

## Who uses it

- [`esp32_p4_display`](../esp32_p4_display/): `lib_deps = file://../esp32_p4_ui`, include path
  `-I${PROJECT_DIR}/../esp32_p4_ui/include`.
- [`tools/gauge_preview`](../../tools/gauge_preview/): builds `src/gauges/*.cpp`, `phone_can.cpp`, `ui_menu.cpp`,
  `ui_theme.cpp` and the fonts for the Mac with LVGL and stubs, and renders faces, the call card and the menu to PNG.

Depends on: LVGL (`^8.3.11` in `library.json`; the app pins `lvgl/lvgl@8.4.0`, and `include/lv_conf.h` is the v8.4.0
config), GFX Library for Arduino (`^1.6.0`; the app pins 1.6.5), [`elc_can`](../elc_can/) and
[`esp32_p4_common`](../esp32_p4_common/).

## Status

- No host unit tests in this directory. The faces, the call card and the menu render on a Mac through
  `tools/gauge_preview` with the same code.
- On hardware: the root [`README.md`](../../README.md#status) says the screens run on Waveshare P4 boards on the
  bench in all three positions, with the themes, menu, media, phone, call card and accessory pages. Nothing is in the
  car.

## Layout

| File | Role |
| --- | --- |
| `include/p4_screen.h`, `src/p4_screen.cpp` | LVGL owns the panel: strips rendered in on-chip RAM into the hidden one of two DSI framebuffers, flip on refresh, redrawn rows copied to the other buffer by the AXI DMA (`esp_async_memcpy`). The optional legacy Arduino_GFX canvas layer (center only), the gauge layer, the UI layer. Tear-free on / off, direct mode for the camera, `perf` statistics, the `shot` screenshot stream. Also the LVGL PSRAM allocator (`lv_psram_malloc()`) |
| `include/lv_conf.h`, `include/lv_mem_psram.h` | LVGL 8.4 config (16-bit colour, custom allocator, custom tick, 30 ms refresh period) and the allocator's declarations |
| `include/elc_gauges.h`, `src/gauges/screen.cpp` | The face API: `gauges_begin()`, `gauges_show(theme, face, variant)`, `gauges_update(GaugeData)`. Paints the static dial once per face (5 cached), builds the live parts, the NO DATA overlay |
| `src/gauges/face.h` | `gauge::Face` (paint / build / update), the three factories, small format helpers |
| `src/gauges/faces_classic.cpp` | Classic: Speedo, Tach, Clock, Econ, Media, Right (two layouts: `ClassicRightQuad` = variant 0, `ClassicRightFuel` = variant 1) |
| `src/gauges/faces_modern.cpp` | Modern: the same six faces, flat dark glass with an orange arc |
| `src/gauges/faces_fallout.cpp` | Fallout: the same six faces, green phosphor with segment bars and scanlines |
| `src/gauges/paint.{h,cpp}` | Anti-aliased software painter (signed distance shapes) for `Canvas888` (the static dial, dithered to RGB565) and `LvSurface` (live parts inside an LVGL draw event) |
| `src/gauges/widgets.{h,cpp}` | `Geo` (800-unit design square to pixels), `Smooth`, and the live shapes: `Needle`, `Hub`, `ArcFill`, `Lamp`, `Scanlines`, `ArtDisc` (album art) |
| `include/elc_fonts.h`, `src/fonts/*.c` | The generated LVGL fonts; sources, sizes and licences in [`src/fonts/README.md`](src/fonts/README.md) |
| `include/FreeSansBold10pt7b.h`, `include/FreeSansBold12pt7b.h` | Adafruit-GFX-format bitmap fonts used by the center's older full-screen pages (`center_ui.cpp`) |
| `include/ui_theme.h`, `src/ui_theme.cpp` | Colours and fonts of the overlays (menu, call card) per theme |
| `include/ui_menu.h`, `src/ui_menu.cpp` | The cluster menu: the home wheel and the Brightness, Theme, Screens, Service, Phone and Accessories pages; touch and knob input; `ui_menu_describe()` for the console's texts; the hold-progress ring. Backed by a `UiMenuHost` |
| `include/p4_touch.h`, `src/p4_touch.cpp` | Touch: GT911 at `0x5D` or `0x14`, or CST816 at `0x15`, auto-detected on SDA 7 / SCL 8; a lift counts after 40 ms |
| `include/p4_round_mask.h`, `src/p4_round_mask.cpp` | Blacks out the corners of a square RGB565 buffer (the center's legacy pages) |
| `include/gauge_link.h`, `src/gauge_link.cpp` | `gauge_fill_sensors()`: `GaugeData` from a `SensorLink` (speed from wheel RPM, the tank size from the sensor card's page) |
| `include/gauge_console.h`, `src/gauge_console.cpp` | The side screens' serial console (`shot`, `perf`, `demo`, `theme`, `variant`, `can`, `help`, plus the app's `on_line` hook) |
| `include/cluster_peer.h`, `src/cluster_peer.cpp` | A side screen's view of the center: follows `0x210` / `0x213`, asks for changes with `0x212`, opens the menu on a tap when it is the interface screen, the 3 s hold (media view, or the right screen's Classic layout), brightness, album-art fetch while it shows media |
| `include/media_can.h`, `src/media_can.cpp` | Now playing and album art from the audio bridge (its range, `0x211` requests); the small copy first, then the original; decoded by the P4's hardware JPEG decoder when the IDF has it; the radio's NOW PLAYING for the media face |
| `include/phone_can.h`, `src/phone_can.cpp` | Phone state from the bridge, the call card overlay on the media screen, phone commands `0x215`, and `phone demo …` (synthetic bridge frames for the bench) |
| `include/menu_can.h`, `src/menu_can.cpp` | `0x216` menu input applied on the interface screen; `0x217`–`0x219` state and texts sent from it; a pairing request opens the Phone page; `menu …` console commands |
| `include/chan_screen.h`, `src/chan_screen.cpp` | A screen's node on the channel protocol: ANNOUNCE (display, product `0x0001`), the accessory pages (`accessory_can`), SYSTEM state, the audio card's source and the radio's NOW PLAYING, LOG lines printed on the center; `chan` / `chan query` |

## The loop contract

Each module says its contract in its header. In short, per loop pass on any screen:

1. `ElcCan::poll()` (its frame handler passes each frame to `menu_can_frame()`, `phone_can_frame()`,
   `media_can_frame()` and, on a side screen, `cluster_peer_frame()`), then `chan_screen_poll()`.
2. Fill a `GaugeData` (`gauge_fill_sensors()`, `media_can_fill_radio()` or `media_can_fill()`, the clock on the
   center) and call `gauges_show()` and `gauges_update()`.
3. `cluster_peer_poll()` (side screens) with the touch sample, `menu_can_poll()`, `phone_can_poll()`.
4. `p4_screen_touch()` (center) and `p4_screen_frame()` once.

Layers, bottom to top: the legacy GFX canvas (`p4_screen_begin(panel, true)`, center only), the gauges
(`p4_screen_gauge_parent()`), the UI (`p4_screen_ui_parent()`: menu, call card, hold ring).

## The faces

- Six faces (`GaugeFace`): `Speedo` (left), `Tach`, `Clock`, `Media`, `Econ` (center views), `Right` (fuel, oil,
  temperature, charge). Every face exists in every theme (`GaugeTheme`: 0 Classic, 1 Fallout, 2 Modern, the same
  numbers as the cluster I2C register and CAN `0x210`).
- A face is designed on an 800-unit square (`Geo`) and scales to the panel: 720×720, 800×800, or the centred 600×600 of
  a 1024×600 panel (`gauges_begin(parent, x, y, size)`).
- `paint()` draws the static dial once into an RGB888 canvas, dithered to RGB565 and cached (`gauges_last_paint_ms()`
  says how long it took). `build()` creates live `Shape`s on top; each invalidates only the pixels it changes.
  `update()` runs every frame; needles glide with `Smooth`.
- While `GaugeData::fresh` is false a face that `uses_sensors()` rests at zero and shows NO DATA.

Preview without hardware (needs one PlatformIO build of the display firmware, for LVGL):

```bash
tools/gauge_preview/build.sh
python3 tools/gauge_preview/preview.py faces.png 0,1,0 2,1,0 1,1,0   # theme,face,variant[,nodata[,phone[,menu]]]
```

## Configuration

This library reads no build flags of its own beyond LVGL's (`-DLV_CONF_INCLUDE_SIMPLE` and the include path to
`include/lv_conf.h`, set by the app). Touch pins are arguments (`p4_touch_begin(sda, scl)`, default 7 / 8). The
framebuffer count comes from `ELC_DSI_NUM_FBS` (the app sets 2) through the GFX patch in
[`esp32_p4_common`](../esp32_p4_common/README.md#build).

Console commands the modules answer (wired in by the app): `phone`, `phone demo idle|discover|incoming|active [muted]
[onphone]|waiting|held|pair|off` (`phone_can_command()`); `menu`, `menu rotate <n>|press|back|open <page>`
(`menu_can_command()`); `chan`, `chan query` (`chan_screen_command()`); and on the side screens `shot`, `perf`,
`demo on|off`, `theme 0|1|2`, `variant 0|1`, `can`, `help` (`GaugeConsole`).

## How it talks to the rest of the system

| Frames | Module | Spec |
| --- | --- | --- |
| `0x210` cluster state, `0x213` center status, `0x212` setting requests | `cluster_peer` | [`can_bus.md`](../../docs/protocols/can_bus.md#cluster-settings) |
| `0x216` menu input, `0x217` menu state, `0x218` / `0x219` menu texts | `menu_can`, `ui_menu` | [`can_bus.md`](../../docs/protocols/can_bus.md#messages) |
| Audio bridge range: transport, text, art info / data; `0x211` art requests | `media_can` | [`can_bus.md`](../../docs/protocols/can_bus.md#audio-bridge-node-type-1) |
| Audio bridge range: phone status, text, pairing; `0x215` phone commands | `phone_can` | [`can_bus.md`](../../docs/protocols/can_bus.md#phone) |
| NET, UI, SYSTEM, AUDIO_CMD (STATUS, NOW PLAYING), LOG | `chan_screen`, `accessory_can` | [`can_channels.md`](../../docs/protocols/can_channels.md) |
| Screenshot stream on the serial port | `p4_screen_shot()` | protocol in `p4_screen.h`; client `tools/screenshot.py` |

## Extending it

**A new gauge face or a change to one.** Faces live in `src/gauges/faces_<theme>.cpp`.

1. Write a class derived from `gauge::Face`: `paint(Canvas888 &, const Geo &)` for the static dial,
   `build(lv_obj_t *, const Geo &)` to `add<Needle>()`, `add<ArcFill>()`, labels and so on (at most 32 shapes),
   `update(const GaugeData &, float dt)` to move them. Override `uses_sensors()` to return false for a face that
   needs no sensor data (the clock, media).
2. Return it from that theme's factory (`make_classic`, `make_modern`, `make_fallout`). A face must exist in all three
   themes.
3. A new face kind also needs a `GaugeFace` value (append it; the preview tool numbers faces by position) and whatever
   the app does to select it.
4. Render it with `tools/gauge_preview` before flashing; check `perf` on the board for the paint time.

**New data on a face:** add a field to `GaugeData` (with a resting default), fill it in the app or in
`gauge_fill_sensors()` / `media_can_fill()`, read it in `update()`.

**A new font size:** generate it with `lv_font_conv` as in [`src/fonts/README.md`](src/fonts/README.md), add the
`LV_FONT_DECLARE` to `include/elc_fonts.h`, and add a row to the fonts README.

**A new menu page:** add a `UiMenuPage` value at the end (the numbers go on CAN in `0x216` / `0x217` as
`ElcCanMenuPage`, so keep them stable and add it to `elc_can_codec.h` and `can_bus.md`), build the page in
`ui_menu.cpp`, and give it a `ui_menu_describe()` text for the console. Anything the page reads or changes goes
through `UiMenuHost` so the center and the side screens can each back it.

## Known issues

- `include/menu_can.h` documents `menu open <page 0..6>`; `menu_can.cpp` accepts any page below `kUiMenuPages` (8),
  so 7 (Accessories) works too.
- `include/cluster_peer.h` still names `0x214` as part of the center status; `0x214` is reserved and nothing sends it.
- [`src/fonts/README.md`](src/fonts/README.md) says the icon code points are in `center_menu.cpp`; they are used in
  `src/ui_menu.cpp`, `src/phone_can.cpp` and the Classic and Modern faces.
- `include/FreeSansBold10pt7b.h` and `FreeSansBold12pt7b.h` carry no source or licence note. They are in the
  Adafruit GFX font format (GNU FreeFont glyphs); the licence should be recorded before release.
