# OLED simulator (legacy)

**Status: legacy.** This simulates the OLED of the RP2040 encoder controller
([`arduino/sketches/encoder_controller`](../../arduino/sketches/encoder_controller/)) from the shelved Raspberry Pi
version of the dash (git tag `pi-pygame-final`). The current console is the ESP32-S3 board in
[`firmware/esp32s3_console`](../../firmware/esp32s3_console/), which talks CAN, not this protocol; to preview its screens
use [`tools/console_preview`](../console_preview/). This tool is kept for work on the old controller.

It opens a window that draws the 256 × 64 SSD1322 OLED the way `encoder_controller.ino` does, from the same Pi → controller
UART lines (`MODE=…|TITLE=…|…`, [`docs/protocols/nano_uart.md`](../../docs/protocols/nano_uart.md)). Lines come from a
serial port or from standard input.

## Needs

- Python 3
- `pygame` (the window)
- `freetype-py` (`import freetype`; reads the BDF fonts)
- `pyserial`, only for `--port`

```bash
pip install pygame freetype-py pyserial
```

## Usage

```
python3 tools/oled_sim/oled_sim.py (--port PORT | --stdin) [--baud N] [--scale N] [--fps N]
```

| Argument | Default | Meaning |
| --- | --- | --- |
| `--port PORT` | | Serial device to read lines from (one of `--port` / `--stdin` is required) |
| `--stdin` | | Read newline-delimited lines from standard input |
| `--baud N` | `115200` | Serial rate |
| `--scale N` | `4` | Integer upscale of the 256 × 64 window |
| `--fps N` | `60` | Redraw rate |

Close the window or press Escape to quit.

```bash
python3 tools/oled_sim/oled_sim.py --port /dev/ttyUSB0
python3 tools/oled_sim/oled_sim.py --port /dev/serial0 --scale 4
while true; do echo 'MODE=drive|TITLE=|WL=OUTSIDE|WT=72F|ST=Song|SA=Artist|PR=40|PE=1:30|PT=3:45|TM=10:23|AP=AM'; sleep 1; done \
  | python3 tools/oled_sim/oled_sim.py --stdin
```

## What it draws

A line is taken only if it contains `MODE=`, `TITLE=` and `|`, as in the firmware. Fields are `KEY=value` pairs
separated by `|`; text is limited to printable ASCII and cut to the firmware's lengths.

- `MODE=menu`: the menu. With `MT` (item count) set, a scrolling list: `TITLE` in the top bar, rows `M0`–`M2` with
  values `V0`–`V2`, `MO` the first visible row, `MG` the selected row. Without `MT`, the older three lines `TITLE`, `L1`,
  `L2`.
- Any other mode: the drive dashboard: `WL` / `WT` (outside label and temperature), `ST` / `SA` (song title and
  artist), `PR` (progress 0–100), `PE` / `PT` (elapsed / total), `TM` / `AP` (clock and AM / PM).
- No valid line for 3 s (or none yet): the idle bowtie graphic.

The window shows lit pixels in warm amber on a dark background. Nothing is written to disk.

| File | |
| --- | --- |
| `oled_sim.py` | The window, the serial / stdin reader, the main loop |
| `renderer.py` | The line parser and the drawing (a port of the sketch's layout) |
| `fonts/` | The three BDF fonts the sketch uses through U8g2 |

## Fonts

The fonts in `fonts/` are standard X11 bitmap fonts, the same ones U8g2 builds its `6x10`, `10x20` and `helvB08` fonts
from. Their terms, from the file headers:

| File | Font | Terms |
| --- | --- | --- |
| `6x10.bdf` | `-Misc-Fixed-Medium-R-Normal--10-100-75-75-C-60-ISO10646-1` | Public domain ("Public domain terminal emulator font. Share and enjoy.") |
| `10x20.bdf` | `-Misc-Fixed-Medium-R-Normal--20-200-75-75-C-100-ISO10646-1` | Public domain ("Public domain font. Share and enjoy.") |
| `helvB08.bdf` | `-Adobe-Helvetica-Bold-R-Normal--11-80-100-100-P-60-ISO10646-1` | Copyright 1984–1989, 1994 Adobe Systems Incorporated and 1988, 1994 Digital Equipment Corporation; permission to use, copy, modify, distribute and sell, provided the copyright and permission notices are kept (the notice is in the file's header comments, which must stay). ISO 10646-1 extension by Markus Kuhn, 2001 |
