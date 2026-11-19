# Console preview

Renders the console's 256 × 64 OLED screens on the host and tiles them into one PNG, lit amber like the panel. The
console firmware's own UI code (`firmware/esp32s3_console/src/console_ui.cpp`, `console_screens.cpp`, `oled_gfx.cpp`),
its fonts and the parts of `elc_can` it needs are compiled for the Mac (or Linux) with clang. For each scenario the
renderer feeds the UI the CAN frames and knob input of a bench situation, then draws one frame. Use it to change a
screen without flashing the console. The console itself is described in
[`firmware/esp32s3_console/README.md`](../../firmware/esp32s3_console/README.md).

## Build

Needs clang / clang++ (C++17), Python 3 and Pillow (`pip install pillow`).

Only LVGL's font code is used (`lv_font.c`, `lv_font_fmt_txt.c`, `lv_utils.c`). It comes from a PlatformIO build of the
console firmware, so build that once first:

```bash
cd firmware/esp32s3_console && pio run -e console && cd -
tools/console_preview/build.sh
```

`build.sh` looks for LVGL in `firmware/esp32s3_console/.pio/libdeps/console/lvgl`; set `LVGL_DIR` to use another
copy. It writes the objects and `build/render` to `tools/console_preview/build/` (ignored by git). The C objects are
recompiled when their source is newer; the C++ sources on every run.

| File | |
| --- | --- |
| `build.sh` | Compiles the font objects and the console sources into `build/render` |
| `render.cpp` | The scenarios: the CAN frames and knob input fed to `ConsoleUi`, then one frame written as a PGM |
| `preview.py` | Runs the renderer for each scenario, lights the frames amber, and tiles them with their names |

## Usage

```
python3 tools/console_preview/preview.py [OUT.png] [THEME] [SCENARIO ...]
```

| Argument | Default | Meaning |
| --- | --- | --- |
| `OUT.png` | `console.png` | Output file |
| `THEME` | `0` | `0` Classic, `1` Fallout, `2` Modern (the console follows the cluster's theme) |
| `SCENARIO` | every scenario in `preview.py`'s list | One or more of the names below |

```bash
python3 tools/console_preview/preview.py                                  # every scenario, Classic, console.png
python3 tools/console_preview/preview.py modern.png 2                     # every scenario, Modern
python3 tools/console_preview/preview.py calls.png 0 call-in call-active pair
```

### Scenarios

Unless noted, the console has the center's theme, the sensor frames (about 45 mph, 58 % fuel), a connected audio
bridge playing a track, and the time (10:23 local).

| Scenario | Shows |
| --- | --- |
| `boot` | Mid-way through the power-up animation |
| `media` | Drive page, Bluetooth playing, phone connected (signal 4, battery 3) |
| `media-low` | The same with a weak signal and low battery (not in the default list) |
| `radio` | Drive page with the simulated radio card as the source |
| `hints` | The radio after a turn of the right knob (the soft-key labels lit) |
| `presets` | The preset list, highlight moved |
| `trip`, `engine`, `clock` | The next drive pages (right soft key once, twice, three times) |
| `menu` | The menu's top page |
| `menu-radio` | The menu's Radio page, with the simulated radio card |
| `menu-phone` | The Phone page with two paired phones, one connected |
| `menu-display` | The Display page |
| `edit` | A value editor |
| `source` | After holding the left knob (next source) |
| `sound` | The audio card's Sound page |
| `eq`, `eq-edit` | The Equalizer, and one band being edited |
| `aux` | The Sound page with aux in as the source |
| `pair` | A phone asking to pair, with its code |
| `alert` | A low oil pressure warning |
| `call-in` | An incoming call |
| `call-active` | A call in progress, muted, audio in the car |
| `volume` | After turning the left knob |
| `toast` | A short notice after the center view changes |
| `nophone` | The audio bridge heard, but no phone connected |
| `reverse` | Reversing (both soft keys blank) |
| `waiting` | No frames from the cluster for 4 s |

The phone names, caller, stations and track text are fixed sample strings in `render.cpp`.

## Output

One PNG with two tiles per row. Each tile is the 256 × 64 frame scaled 3× with a pixel grid and a slight glow,
coloured amber, with the scenario's name above it. `preview.py` prints the output path.

The renderer can be run on its own; it writes the 16-grey frame as a binary PGM (levels 0–15 scaled to 0–255):

```
build/render SCENARIO THEME OUT.pgm
```
