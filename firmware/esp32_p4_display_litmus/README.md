# esp32_p4_display_litmus: MIPI panel bring-up test

The smallest firmware that lights a MIPI DSI panel on an ESP32-P4 board: it starts the panel, copies the boot splash
into the framebuffer and writes HELLO in the middle. No sensors, no LVGL, no CAN, no touch. Use it to check a new
panel, board or chip revision before flashing [`esp32_p4_display`](../esp32_p4_display/), or to tell a dead panel from
a dead board. `litmus-serial-only` drops the display too and only prints on the serial port.

Overview of all the firmware: [`../README.md`](../README.md).

```bash
./flash.sh -e litmus-waveshare-3c4 --upload-port /dev/cu.usbmodem…
```

## Status

A bench tool; it is not part of the product. Which envs have been run on which boards is not stated, apart from the
troubleshooting notes in `platformio.ini` (the 3.4C's two USB-C ports, chip revisions and boot loops).

## Hardware it targets

| Env | Board JSON | Panel | Serial |
| --- | --- | --- | --- |
| `litmus-waveshare-3c4` | `esp32-p4-waveshare-wifi6-touch-lcd-3c4` (PSRAM, `esp32p4_es`) | Waveshare 3.4C, 800×800 SH8601 | USB CDC |
| `litmus-waveshare-3c4-uart` | same | same | UART0 |
| `litmus-waveshare` | `esp32-p4-evboard-nopsram` | Waveshare 4" 720×720 JD9365 (PMIC, no backlight GPIO) | USB CDC |
| `litmus-waveshare-uart` | `esp32-p4-evboard-nopsram` | same | UART0 |
| `litmus-ev` | `esp32-p4-evboard-nopsram` | EV board + 720×720 JD9165 module | USB CDC |
| `litmus-ev-uart` | `esp32-p4-evboard-nopsram` | same | UART0 |
| `litmus-ev-1024x600` | `esp32-p4-evboard-nopsram` | EV board's stock 7" 1024×600 | USB CDC |
| `litmus-ev-1024x600-uart` | `esp32-p4-evboard-nopsram` | same | UART0 |
| `litmus-serial-only` | `esp32-p4-evboard-nopsram` | none | UART0 |

Board JSONs: [`../boards/`](../boards/README.md). Pick the env for the panel actually fitted: the 3.4C and the 4"
profiles differ in resolution and init and a wrong one leaves the panel black.

## Layout

| File | Role |
| --- | --- |
| `src/main.cpp` | `setup()`: logs, `create_litmus_display()`, the boot splash, HELLO, flush. With `LITMUS_SERIAL_ONLY`: ticks on the serial port, no display |
| `src/display_init.{h,cpp}` | `create_litmus_display()`: the same panel profiles as `esp32_p4_common/src/display_setup.cpp` (timings, lane rate, reset, PMIC power-on, backlight) under `LITMUS_*` names |
| `src/waveshare_3inch4_dsi_800_init.h`, `src/waveshare_jd9365_720_init.h` | Declarations of the init tables; the tables are compiled from `../esp32_p4_common/src/` (`build_src_filter`) |
| `platformio.ini` | The envs and the troubleshooting notes |
| `flash.sh`, `requirements.txt` | Local venv with PlatformIO (`platformio>=6.1.18`), then `platformio run -t upload` |

## Build and flash

```bash
cd firmware/esp32_p4_display_litmus
./flash.sh -e litmus-waveshare-3c4 --upload-port /dev/cu.usbmodem…
./flash.sh --erase-first -e litmus-waveshare-3c4 --upload-port /dev/cu.usbmodem…   # full erase, then upload
.venv/bin/python -m platformio device monitor -e litmus-waveshare-3c4 --port /dev/cu.…
```

`flash.sh` is the display firmware's script with litmus help text: it creates `.venv` (Python 3.10 or newer from
`PATH`), installs `requirements.txt`, checks the RISC-V toolchain and runs PlatformIO. Every env runs
[`../scripts/pio_pre_build.py`](../scripts/pio_pre_build.py); all but `litmus-serial-only` pull in
[`esp32_p4_common`](../esp32_p4_common/) (for the boot splash) and GFX Library for Arduino 1.6.5.

What the serial port says (from `main.cpp`):

| Output | Meaning |
| --- | --- |
| `[litmus] display OK — drawing on WxH`, then `litmus: drew "HELLO" …` | the panel started; a black screen now points at the backlight or the MIPI link |
| `[litmus] FATAL: create_litmus_display() returned nullptr` | the panel did not start: init table or timing |
| nothing | wrong port or console: try the `-uart` env or `litmus-serial-only` |

`platformio.ini` has notes on boot loops (`Illegal instruction` at `entry 0x4ffac2c0` on eco2 silicon means the wrong
`chip_variant`: run `pio run -t fullclean` and flash with `--erase-first` after changing it) and on
`CHIP_LP_WDT_RESET`.

## Configuration (build flags)

| Flag | Default | Meaning |
| --- | --- | --- |
| `LITMUS_WAVESHARE_3C4` | off | 800×800 SH8601 |
| `LITMUS_USE_JD9365` | off | 720×720 JD9365 |
| `LITMUS_PANEL_1024X600` | off | EV 7" 1024×600 |
| (none) | | EV 720×720 JD9165 |
| `LITMUS_SERIAL_ONLY` | off | no display |
| `LITMUS_LCD_RST` | 27 (none on JD9365) | panel reset |
| `LITMUS_BACKLIGHT_PIN` | 26 | `-1` = PMIC only |
| `LITMUS_BACKLIGHT_USE_PWM` / `_ACTIVE_LOW` / `_LEVEL` | 1 / 0 / 51 | backlight |
| `LITMUS_WAVESHARE_PMIC_I2C_ADDR` / `_SDA` / `_SCL` | `0x45` / 7 / 8 | Waveshare PMIC |

`boot_splash.cpp` (in `esp32_p4_common`) picks the splash size from `LITMUS_WAVESHARE_3C4` and
`LITMUS_PANEL_1024X600` as well as the `TSPS_*` flags.

## Extending it

A new panel goes here first: add a profile block to `display_init.cpp` (and the init table to
`esp32_p4_common/src/` if it needs one), an env to `platformio.ini`, and once it lights, the same profile to
`esp32_p4_common/src/display_setup.cpp`.

## Known issues

- [`../README.md`](../README.md) describes this as "solid color / HELLO" and `platformio.ini` mentions a "red fill";
  the code draws the boot splash and HELLO, with no fill.
- Not checked by building: `esp32_p4_common`'s `sensor_link.cpp` and `sensor_packet.cpp` include headers from
  `../elc_can/include`, which the litmus envs neither list in `lib_deps` nor put on the include path (the display
  firmware does both). If a litmus build stops on `elc_can_codec.h` or `elc_sensor_units.h`, that is why.
- `display_init.cpp` duplicates the panel profiles of `esp32_p4_common/src/display_setup.cpp` (without the 3.4C's
  48 MHz DPI clock the display firmware uses); a change to one has to be made to the other by hand.
