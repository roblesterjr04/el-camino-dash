# boards: custom PlatformIO board definitions (and the shared build helpers)

PlatformIO board JSONs for the boards that PlatformIO does not ship, or ships with settings that do not fit. A
project uses them with `boards_dir = ../boards` in its `[platformio]` section and `board = <file name without .json>`
in an env. The last section covers the other shared build files in `firmware/`: `scripts/pio_pre_build.py`,
`flash_display.sh` and `requirements-pio.txt`.

Overview of all the firmware: [`../README.md`](../README.md).

## The boards

| File | Board | Used by |
| --- | --- | --- |
| `esp32-p4-waveshare-wifi6-touch-lcd-3c4.json` | Waveshare ESP32-P4-WIFI6-Touch-LCD-3.4C | `esp32_p4_display` env `display-waveshare3c4`; `esp32_p4_display_litmus` envs `litmus-waveshare-3c4`, `-uart` |
| `esp32-p4-evboard.json` | Espressif ESP32-P4 Function EV board (with PSRAM) | `esp32_p4_display` envs `display-waveshare4c`, `display-ev720`, `display-ev7` |
| `esp32-p4-evboard-nopsram.json` | the same board without `BOARD_HAS_PSRAM` | `esp32_p4_display_litmus` (every env except the 3.4C ones) |
| `sensorcard2_g0b1kb.json` | the rev B sensor card, STM32G0B1KBU6 | `sensor_card_stm32` env `sensorcard2` |

### esp32-p4-waveshare-wifi6-touch-lcd-3c4

ESP32-P4, `chip_variant` `esp32p4_es` (the 3.4C's early silicon needs the Arduino "before v3.00" prebuilts), QIO
flash at 80 MHz, 32 MB flash with `default_32MB.csv`, PSRAM at 200 MHz (`-DBOARD_HAS_PSRAM`), CPU 360 MHz, upload at
1500000 baud. The display env sets the same chip variant, flash mode and clock again and adds its own pin variant
(`esp32_p4_display/variants/esp32p4/pins_arduino.h`) for the TF slot.

### esp32-p4-evboard

ESP32-P4, `chip_variant` `esp32p4` (retail silicon), QIO flash at 80 MHz, 16 MB flash with `default_16MB.csv`, PSRAM
at 200 MHz (`-DBOARD_HAS_PSRAM`), CPU 360 MHz. The 4C env overrides the CPU to 400 MHz (rev 3 silicon rejects 360).
The Waveshare 4C uses this JSON as well (see `esp32_p4_display/platformio.ini`).

### esp32-p4-evboard-nopsram

As `esp32-p4-evboard`, without `BOARD_HAS_PSRAM` and `f_psram`. Its name says why: the litmus test and
`CHIP_LP_WDT_RESET` debugging, where PSRAM bring-up is a suspect (`esp32_p4_display_litmus/platformio.ini`).

### sensorcard2_g0b1kb

STM32G0B1KBU6 (Cortex-M0+, 64 MHz), STM32duino's generic `G0B1K(B-C-E)(T-U)` variant with
`ARDUINO_GENERIC_G0B1KBUX`, 128 KB flash, 144 KB RAM, ST-Link upload by default (also CMSIS-DAP, J-Link, Black Magic,
serial), `stlink` for debugging. Details and the reasons: [`../sensor_card_stm32/README.md`](../sensor_card_stm32/README.md).

### Adding a board

Copy the closest JSON, change `name`, `url`, the MCU and memory fields, and point the env at it with
`board = <name>`. Keep `boards_dir = ../boards` in the project. For a new ESP32-P4 board, check the chip revision
first: the wrong `chip_variant` gives a boot loop (`esp32_p4_display_litmus/platformio.ini` describes the symptom),
and after changing it run `pio run -t fullclean` and flash with an erase.

## Shared build helpers in `firmware/`

### `scripts/pio_pre_build.py`

A PlatformIO pre-script (`extra_scripts = pre:../scripts/pio_pre_build.py`) used by every env of
[`esp32_p4_display`](../esp32_p4_display/) and [`esp32_p4_display_litmus`](../esp32_p4_display_litmus/). On each
build it:

1. **Finds the RISC-V compiler.** pioarduino may install `riscv32-esp-elf-g++` under
   `~/.platformio/tools/toolchain-riscv32-esp*` while `packages/` holds an empty stub; the script searches both
   (`PLATFORMIO_CORE_DIR` if set) and puts the first `bin/` with the compiler on `PATH`. If none has it, it deletes the
   incomplete toolchain directories, writes `.pio/toolchain_reinstall_retry` and stops; `flash.sh` sees that file and
   runs PlatformIO once more so the toolchain downloads again (with `pio` directly, run the command again).
2. **Refreshes the local libraries.** It copies `../esp32_p4_ui`, `../esp32_p4_common` and `../elc_can` over their
   copies in `.pio/libdeps/<env>/` (`el_camino_p4_ui`, `el_camino_p4_common`, `elc_can`), because a `file://`
   dependency is copied once and goes stale.
3. **Patches GFX Library for Arduino.** It imports `../esp32_p4_common/scripts/pio_pre_gfx_dsi_io_hook.py` and applies
   its three patches to `Arduino_ESP32DSIPanel.cpp` in `.pio/libdeps` (panel IO handle, framebuffer count and panel
   handle, DSI PHY clock on rev ≥ 3 silicon). See [`../esp32_p4_common/README.md`](../esp32_p4_common/README.md#build).

### `flash_display.sh`

Flashes any round screen (one image for all three positions) by calling `esp32_p4_display/flash.sh`:

```bash
UPLOAD_PORT=/dev/cu.usbmodem… ./flash_display.sh                              # display-waveshare3c4
ENV=display-waveshare4c UPLOAD_PORT=/dev/cu.usbmodem… ./flash_display.sh      # 4" panel
```

Extra arguments are passed on to `flash.sh` (for example `--erase-first`). Without `UPLOAD_PORT` it uses a fixed
`/dev/cu.usbmodem…` name that belongs to one bench board; set `UPLOAD_PORT`.

### `requirements-pio.txt`

Python packages (`littlefs-python>=0.17`, `fatfs-ng>=0.1`, `PyYAML>=6`) that the pioarduino (Espressif 55.x) builder
imports. Install them into the same Python that runs PlatformIO (`pio system info` shows it), for example when
PlatformIO comes from Homebrew rather than a project's `.venv`:

```bash
python3 -m pip install -r firmware/requirements-pio.txt
```

It does not install PlatformIO itself. The `flash.sh` scripts make their own `.venv` from their project's
`requirements.txt` (`platformio>=6.1.18`, plus `littlefs-python` for the display firmware).

## Known issues

- The board names disagree on the P4 silicon: `esp32-p4-evboard.json` says `esp32p4` "for eco2 / retail; not
  esp32p4_es", and `esp32-p4-waveshare-wifi6-touch-lcd-3c4.json` says "eco2 rev1: chip_variant esp32p4_es".
  `esp32_p4_display_litmus/platformio.ini` repeats both.
- `sensorcard2_g0b1kb.json` has a `url` with the repository owner's GitHub account; update it if the repository
  moves.
- The root [`README.md`](../../README.md#prerequisites) offers `pip install -r firmware/requirements-pio.txt` as a way
  to get PlatformIO, but the file does not list `platformio`.
