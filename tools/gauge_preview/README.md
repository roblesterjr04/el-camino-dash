# Gauge preview

Renders the round displays' gauge faces on the host with the firmware's own LVGL code, and tiles them into one PNG.
The same sources that run on the ESP32-P4 are compiled for the Mac (or Linux) with clang: the faces
(`firmware/esp32_p4_ui/src/gauges/`), the call card (`phone_can.cpp`), the menu (`ui_menu.cpp`, `ui_theme.cpp`), the
shared LVGL fonts, and the parts of `elc_can` they need. Use it to change a layout without flashing a board.

## Build

Needs clang / clang++ (C++17), Python 3 and Pillow (`pip install pillow`).

LVGL comes from a PlatformIO build of the display firmware, so build that once first (it fetches LVGL 8.4 into
`.pio/libdeps/`):

```bash
cd firmware/esp32_p4_display && pio run -e display-waveshare3c4 && cd -
tools/gauge_preview/build.sh
```

`build.sh` looks for LVGL in `firmware/esp32_p4_display/.pio/libdeps/display-waveshare3c4/lvgl`. To use another copy,
set `LVGL_DIR`:

```bash
LVGL_DIR=/path/to/lvgl tools/gauge_preview/build.sh
```

It writes the objects and the renderer to `tools/gauge_preview/build/` (ignored by git). LVGL's objects are compiled
once and kept; delete `build/lvgl/` after changing the LVGL version. The fonts are recompiled when their source is
newer. The firmware sources are recompiled on every run.

| File | |
| --- | --- |
| `build.sh` | Compiles LVGL, the fonts and the firmware sources into `build/render` |
| `render.cpp` | The renderer: sets up an LVGL display in memory, shows one face with sample data, writes a PPM |
| `stubs.cpp` | Host stand-ins for `millis()`, the PSRAM allocator and `chan_screen()` (no accessories are heard) |
| `stub/` | Host versions of `Arduino.h` and `p4_screen.h` (the real `p4_screen.h` needs Arduino_GFX) |
| `preview.py` | Runs the renderer for each spec and tiles the results |

## Usage

```
python3 tools/gauge_preview/preview.py OUT.png SPEC [SPEC ...]
```

Each `SPEC` is comma-separated: `theme,face,variant[,nodata[,phone[,menu]]]`.

| Field | Values |
| --- | --- |
| `theme` | `0` Classic, `1` Fallout, `2` Modern |
| `face` | `0` Speedo (left), `1` Tach, `2` Clock, `3` Media, `4` Econ (center views), `5` Right (fuel / oil / temp / charge) |
| `variant` | Right display, Classic only: `0` classic_1, `1` classic_2. Default `0` |
| `nodata` | `1` renders the face as if sensor data stopped (needles at rest, **NO DATA**). Default `0` |
| `phone` | The call card over the face, as `phone demo <mode>` on a board's console: `idle`, `discover`, `incoming`, `active`, `waiting`, `held`, `pair`. Add modifiers with `+`: `active+muted`, `active+muted+onphone` |
| `menu` | The menu open on a page over the face: `0` Home, `2` Brightness, `3` Theme, `4` Screens, `5` Service, `6` Phone, `7` Accessories (`1` is reserved). Optionally knob input after a colon, separated by `;`: `r<n>` rotate by n, `p` press, `b` back. Example: `6:r2;p` |

Leave a field empty to skip it (`0,1,0,0,,6` opens the menu with no phone demo). With a `menu`, the phone demo runs
underneath it, 25 s in. Without a menu, the call card is shown 4 min 32 s into the call.

Two environment variables change the sample data (they pass through `preview.py` to the renderer):

| Variable | Effect |
| --- | --- |
| `RADIO=1` | The media face shows the radio (station, now playing, preset, signal) instead of a Bluetooth track |
| `ART=1` | The media face gets a synthetic 200 × 200 album cover |

### Examples

```bash
python3 tools/gauge_preview/preview.py faces.png 0,1,0 2,1,0 1,1,0          # the tach in Classic, Modern, Fallout
python3 tools/gauge_preview/preview.py right.png 0,5,0 0,5,1                 # both Classic right layouts
python3 tools/gauge_preview/preview.py nodata.png 0,0,0,1                    # speedo with no sensor data
python3 tools/gauge_preview/preview.py call.png 0,3,0,0,incoming 2,3,0,0,active+muted 1,3,0,0,pair
python3 tools/gauge_preview/preview.py phone.png 0,1,0,0,idle,6 2,1,0,0,pair,6 1,1,0,0,discover,6
python3 tools/gauge_preview/preview.py menu.png "0,1,0,0,,6:r2;p"           # quote a spec that contains ;
RADIO=1 ART=1 python3 tools/gauge_preview/preview.py media.png 0,3,0 2,3,0
```

## Output

One PNG: up to three tiles per row, in the order given. Each tile is 800 × 800 (the 3.4" panel), masked to the round
screen on a grey background. For each spec, `preview.py` prints the renderer's notes on stderr: the time the static
dial took to paint, the pixels in the first frame and in one update step, and the menu's or phone's describe text when
one is shown.

The sample data is fixed in `render.cpp` (`sample()`): 47 mph, 2850 rpm, fuel 64 %, coolant 195 °F, oil 38 psi,
10:08:32, a Bluetooth track, and so on. After the first frame the values move a few steps and the needles are left to
settle before the image is taken.

## The renderer on its own

`build/render` can be run directly; it writes a binary PPM:

```
build/render OUT.ppm THEME FACE VARIANT [W H] [NODATA] [STEPS]
```

`W H` default to 800 × 800. The face is a square the size of the smaller side, centred, so `1024 600` shows the 7"
panel's centred 600 × 600 face.
`STEPS` is the number of update steps after the first frame (`preview.py` passes `2`). The `PHONE` and `MENU`
environment variables take the same values as the `phone` and `menu` fields above, except that `MENU` separates the
keys with `,` instead of `;`.
