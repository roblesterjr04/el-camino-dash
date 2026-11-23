# Tools

Host-side tools for the bench, for previewing the screens without hardware, and for making the boards' fab files.
Everything here runs on a Mac or Linux computer; nothing here is flashed to a board.

| Tool | Group | What it does |
| --- | --- | --- |
| [`can_relay.py`](#can_relaypy) | Bench & simulation | Joins the boards plugged in over USB into one CAN bus; simulates the drive, the clock and the unbuilt cards |
| [`cards_sim.py`](#cards_simpy) | Bench & simulation | Simulated audio, GPS, LTE, voice and transmission-temperature cards (used by `can_relay.py`) |
| [`chan_sim.py`](#chan_simpy) | Bench & simulation | Channel-protocol framing and the base class every simulated card shares |
| [`radio_sim.py`](#radio_simpy) | Bench & simulation | Simulated AM/FM radio card (used by `can_relay.py`) |
| [`screenshot.py`](#screenshotpy) | Bench & simulation | Saves what a round display shows as a PNG, over its USB console |
| [`fw_update.py`](#fw_updatepy) | Bench & service | Sends an `.elcfw` firmware file over CAN to every board of its build (or one node), from a USB-CAN adapter or a board's USB link |
| [`gauge_preview/`](gauge_preview/) | Previews | Renders the gauge faces, call card and menu on the host with the firmware's own code |
| [`console_preview/`](console_preview/) | Previews | Renders the console OLED's screens on the host with the console firmware's own code |
| [`oled_sim/`](oled_sim/) | Previews (legacy) | Window that draws the old encoder controller's OLED from its UART lines (Raspberry Pi era) |
| [`fab_export.py`](#fab_exportpy) | Fabrication | Gerbers, drill, placement, centroid and JLCPCB CPL for a board |
| [`mask_dams.py`](#mask_damspy) | Fabrication | Narrows fine-pitch SMD pads so a solder-mask dam fits between them |
| [`smd_only.py`](#smd_onlypy) | Fabrication | SMD-only BOM and placement files for the assembler |

Python 3 for all of them. Extra packages, where needed, are listed with each tool.

## Bench & simulation

The bench has no CAN wiring: each board carries its CAN traffic over its USB serial port when a relay asks for it, and
`can_relay.py` copies the frames between the boards. The link is part of the shipped firmware; there are no bench
builds. Wire format and behaviour: [`docs/protocols/usb_link.md`](../docs/protocols/usb_link.md). The CAN messages
are in [`docs/protocols/can_bus.md`](../docs/protocols/can_bus.md), the accessory channel protocol the simulated
cards speak in [`docs/protocols/can_channels.md`](../docs/protocols/can_channels.md).

### fw_update.py

The service tool (node `0x1F`) for [firmware updates without USB](../README.md#firmware-updates-without-usb): it sends
an `.elcfw` file (PlatformIO writes one next to every `firmware.bin`) as a BULK firmware WRITE, the same transfer the
console's Wi-Fi page makes ([the firmware profile](../docs/protocols/can_channels.md#the-firmware-profile-kind-2)).
Every node of the file's product and build takes it at once; `--dest` picks one node. It prints each node's result and
exits 0 only if every node that took the file updated.

| Option | Default | |
| --- | --- | --- |
| `file` | | The `.elcfw` file |
| `--can IFACE[:CHANNEL]` | | A USB-CAN adapter through [python-can](https://python-can.readthedocs.io/) (`pip install python-can`): `socketcan:can0`, `slcan:/dev/ttyACM0`, `pcan:PCAN_USBBUS1`, `gs_usb:0`. 500 kbit/s |
| `--usb PORT` | | A board's USB port as the gateway onto its bus (bench; `pip install pyserial`) |
| `--dest NODE` | every node of the build | One node, e.g. `0x13` for the console |
| `--pace N` | `2.5` | Data frames per millisecond |
| `--list` | | The nodes on the bus, their product and firmware version |
| `--test` | | Builds the firmware's own receiver (`firmware/elc_can/test/host/fw_target_pipe.cpp`) and updates three simulated screens through it, one of them losing frames; no hardware |

```bash
python3 tools/fw_update.py --can socketcan:can0 --list
python3 tools/fw_update.py --can socketcan:can0 display-waveshare4c.elcfw   # the left and right screens
python3 tools/fw_update.py --usb /dev/cu.usbmodem1101 --dest 0x13 console.elcfw
python3 tools/fw_update.py --test
```

### can_relay.py

Opens every board's USB serial port, sends the keepalives that switch on each board's USB link, and copies every CAN
frame a board sends to all the others. Each board's console text is printed with the board's name, and lines you type
go to a board's shell.

It also stands in for what the bench does not have:

- **A simulated drive** (the firmware's 60 s demo cycle: idle, pull away, cruise, pass, stop) as sensor frames
  `0x100`–`0x104` at 20 Hz, in place of the sensor card. The screens use it only while their own sensor input is silent.
- **The computer's clock** as the time frame `0x220` once a second, in place of GPS.
- **The slot cards that are specified but not built**, each speaking the channel protocol as its firmware will
  (heartbeat, NET ANNOUNCE / QUERY / IDENTIFY / RESTART, and its own channels). The values are made up and settings
  live in memory only.

| Card | Slot, node | Module |
| --- | --- | --- |
| `radio`: AM/FM radio card | 2, `0x15` (`--radio-slot`) | `radio_sim.py` |
| `audio`: audio card | 1, `0x14` | `cards_sim.py` |
| `gps`: GPS card | 3, `0x16` | `cards_sim.py` |
| `lte`: LTE gateway | 4, `0x17` | `cards_sim.py` |
| `voice`: AI voice card | 5, `0x18` | `cards_sim.py` |
| `trans`: transmission-temperature card | 6, `0x19` (`--trans-slot`) | `cards_sim.py` |

A slot card's node is `0x14 + slot - 1`. The simulated audio card goes quiet by itself when a real audio sink
announces (the audio card's firmware, or a DevKit bridge). The cards hear each other's frames as well as the boards'.

**When to use it:** any time more than one board is on the bench, or one board needs the rest of the system around
it. In the truck, `--gateway` lets a laptop on one board watch and send on the real bus.

**Needs:** `pyserial` (`pip install pyserial`). `--test` runs without it.

```
python3 tools/can_relay.py [ports ...] [--baud N] [--gateway BOARD] [--frames] [--no-sensors] [--no-time]
                           [--no-radio] [--no-sim CARD[,CARD]] [--radio-slot 2-6] [--trans-slot 1-6] [--test]
```

| Argument | Default | Meaning |
| --- | --- | --- |
| `ports` | every USB serial port | Serial ports to open. Without any, the relay opens every `/dev/cu.usbmodem*`, `/dev/cu.usbserial*`, `/dev/cu.wchusbserial*`, `/dev/cu.SLAB_USBtoUART*`, `/dev/ttyACM*` and `/dev/ttyUSB*`, and picks up boards plugged in later |
| `--baud N` | `2000000` | Fast rate asked of boards with a USB-UART chip after the link starts. A CP210x (DevKitC) is capped at 921600; native USB (the console's ESP32-S3) has no rate |
| `--gateway BOARD` | off | This board (name or port) bridges the computer onto its real CAN bus. Turns off the simulated drive and the clock (the simulated cards stay on) |
| `--frames` | off | Log every frame |
| `--no-sensors` | off | Don't send the simulated drive |
| `--no-time` | off | Don't send the computer's clock |
| `--no-radio` | off | Same as `--no-sim radio` |
| `--no-sim CARD[,CARD]` | none | Simulated cards to leave off: `radio`, `audio`, `gps`, `lte`, `voice`, `trans` |
| `--radio-slot 2-6` | `2` | Slot of the simulated radio card |
| `--trans-slot 1-6` | `6` | Slot of the simulated transmission-temperature card. The spec's example has it in slot 1, which needs `--no-sim audio` |
| `--test` | | Run the relay's records and the simulated cards together, with no hardware, and exit |

```bash
python3 tools/can_relay.py                         # every USB serial port found, peer mode
python3 tools/can_relay.py --frames                # also log the frames
python3 tools/can_relay.py --gateway center        # the center bridges this computer onto the real CAN bus
python3 tools/can_relay.py --no-sensors --no-time  # only relay what the boards send
python3 tools/can_relay.py --no-sim audio,gps      # leave two simulated cards off
python3 tools/can_relay.py --test                  # self-test, no hardware
```

**Typing while it runs.** A line that starts with a board's name goes to that board's shell; `all` sends it to every
board. Boards are named from the node they report: `center`, `left`, `right`, `sensor`, `console`, `bridge` (or
`bridgeNN` when there are several), otherwise `nodeNN`. A node number (`0x14`) or a port name also works. With only
one board open, a line with no name goes to it.

```
center help          a command to one board
all can              to every board
:frames on|off       frame log;  :filter 0x215 0x404  (no ids = all)
:sensors on|off      the simulated drive;  :time on|off  the clock
:sim list            the simulated cards;  :sim on|off CARD  (off: the card goes silent; on: it boots again)
:radio               status | on | off | source on|off | tune 98.3 | seek up|down
:audio               status | source bt|radio|aux|off | volume 0-100 | mute | play <sound 1-10> [priority 0-4] [repeat] |
                     stop [sound] | media play|next|prev | eq <preset 0-6> | band <0-9> <dB>
:gps                 status | fix on|off
:lte                 status | message [text] | refresh | offline | online
:voice               status | say [text]
:trans               status | temp <deg C> (caution at 110, warning at 125) | mode own|center
:ports               the boards and their links (baud, CAN state, frames up / down, bad records)
:quit                (or Ctrl-C) the boards fall back to plain serial within 3 s
```

**Reads / writes:** the serial ports only. Nothing is written to disk.

Notes:

- The relay never toggles DTR or RTS: on the Waveshare P4 boards the CH343's auto-reset circuit reboots the ESP32
  when one changes.
- `--gateway` by name waits about 2.5 s for the boards to report who they are. Use one gateway per bus, and don't
  mix it with peer boards that share that bus, or they hear frames twice.
- Quit the relay before running `screenshot.py`: both use the same port.

### cards_sim.py

The simulated audio, GPS, LTE, voice and transmission-temperature cards that `can_relay.py` runs. It is a module,
not a program; run on its own it prints its description or runs its self-test.

| Card | Class | Slot, node | What it sends and answers |
| --- | --- | --- | --- |
| `audio` | `AudioCard` | 1, `0x14` | AUDIO_CMD STATUS at 1 Hz and on change; PLAY / STOP / VOLUME / MEDIA, DONE when a sound ends, ducking for priority 2 and up; NET RAIL arbitration (one RET holder; the voice card outranks the others, the radio ranks lowest; RET is freed when the holder's heartbeat stops for 3 s); the media keys and encoder 1 on INPUT; a UI page (Chime volume, Fader, Balance, and an Equalizer group with a preset and ten bands). With Source = Radio it forwards play / next / previous to the radio as MEDIA |
| `gps` | `GpsCard` | 3, `0x16` | SENSOR speed (km/h), heading, altitude, satellites, following the relay's simulated drive; a UI page (fix, satellites, speed). No fix for the first 6 s |
| `lte` | `LteGateway` | 4, `0x17` | NET GATEWAY STATUS at 1 Hz; ONLINE PLACE, WEATHER, TIMEZONE, STATUS; a MESSAGE 60 s after start (repeated every 30 s until MESSAGE READ); answers REFRESH, MESSAGE READ and GATEWAY CMD 4 (data cap); a UI page |
| `voice` | `VoiceCard` | 5, `0x18` | Asks the audio card for RET on `say`, speaks for 3 s once granted, then releases it |
| `trans` | `TransTempCard` | 6, `0x19` | SENSOR 0 trans temp at 5 Hz; its own ALERTs (caution "Trans fluid hot", warning) with STATE at 1 Hz, or with `mode center` the center-raised alerts (SENSOR flag bit 2) instead; UI item 0 Offset |

The sensor card is not simulated here: it is real hardware, and the relay's simulated drive covers its frames.

```bash
python3 tools/cards_sim.py --test     # every card's logic, no hardware
```

### chan_sim.py

What every simulated card shares: the channel IDs and registries, transfer framing (tail byte, multi-frame with
CRC-16/CCITT-FALSE), reassembly, and `SimCard`, a base class that does what every node does (heartbeat
`0x700 + node` at 1 Hz, NET ANNOUNCE at boot and on QUERY after node × 8 ms, the descriptions QUERY asks for,
IDENTIFY and RESTART). A card adds its own channels by overriding the hooks `receive()`, `heard()`, `on_rail()`,
`periodic()`, `sensor_defines()`, `alert_defines()`, `items()`, `command()` and `status()`.

Use it when adding a new simulated card: subclass `SimCard`, then add it to `SIM_CARDS` and `Relay.make_card()` in
`can_relay.py`.

```bash
python3 tools/chan_sim.py --test     # framing and the base card, no hardware
```

### radio_sim.py

The simulated AM/FM radio card ([`docs/hardware/radio_card.md`](../docs/hardware/radio_card.md)). It sends what the
card's firmware will send: heartbeat (node type 9), NET ANNOUNCE and RAIL requests to the audio card, its UI page
(Menu > Accessories > its name), SENSOR 0 signal (dBµV) and 1 SNR (dB) every second, and LOG lines for tune and seek
results. It acts on NET, UI SET / VIEW, AUDIO_CMD STATUS and MEDIA addressed to it, and SYSTEM POWER. With no audio
card heard it acts on the INPUT keys itself while it is the source. Denied or withdrawn RET, it asks again every 2 s.
The stations are made up; every run starts at factory defaults.

```bash
python3 tools/radio_sim.py --test     # transfer framing and the radio's own logic, no hardware
```

### screenshot.py

Sends `shot` to a round display's console, follows its switch to a fast baud rate, reads the raw RGB565 framebuffer
and saves a PNG. Checks the frame against the CRC-32 the board sends and warns on a mismatch.

**When to use it:** to capture what a P4 display shows, for documentation or to compare a layout with the host
preview.

**Needs:** `pyserial`. `Pillow` for a PNG; without it a `.ppm` is written.

```
python3 tools/screenshot.py [--port PORT] [--baud N] [--timeout S] [-o OUT] [--legacy]
```

| Argument | Default | Meaning |
| --- | --- | --- |
| `--port` | first of `/dev/cu.usbmodem*`, `/dev/cu.usbserial*`, `/dev/ttyACM*`, `/dev/ttyUSB*` | The display's USB serial port |
| `--baud` | `115200` | Rate to open the port at (the board then names the fast rate) |
| `--timeout` | `30.0` | Seconds before giving up |
| `-o`, `--out` | `screenshots/<YYYYmmdd-HHMMSS>.png` (relative to the current directory) | Output file |
| `--legacy` | off | Dump the center's legacy Arduino_GFX drawing layer (`shot legacy`) instead of the panel |

```bash
python3 tools/screenshot.py                                   # auto-detect the port
python3 tools/screenshot.py --port /dev/cu.usbmodem… -o center.png
```

**Reads / writes:** the serial port; writes one image file (and creates its directory).

Like the relay, it never toggles DTR or RTS. Quit `can_relay.py` first.

## Previews

The gauge faces and the console screens are drawn by code that also builds on the host. The previews compile that
code with clang and render scenarios to PNG, so layouts can be changed without flashing a board. Each needs one
PlatformIO build of the matching firmware first, to fetch LVGL.

- [`gauge_preview/`](gauge_preview/): the round displays' faces in every theme, the call card and the menu.
- [`console_preview/`](console_preview/): the console's 256 × 64 OLED screens, lit amber like the panel.
- [`oled_sim/`](oled_sim/) (legacy): a live window for the old RP2040 encoder controller's OLED protocol from the
  Raspberry Pi version. The ESP32-S3 console replaced it; use `console_preview` for the current console.

## Fabrication

The boards' KiCad files are generated (`<Board>/generate/`); these scripts turn a routed board into what the fab
needs. They take board directory names (for example `SensorCard`) relative to the repository root, so they run from
any directory.

### fab_export.py

Writes a board's fab outputs into `<Board>/fab/`. Coordinates are measured from the drill / place origin, which each
board's `gen_pcb.py` sets to the board's bottom-left corner. Not-fitted (DNP) parts are left out of the placement
files. On the boards whose through-hole parts are soldered by hand (`HAND_THT` in the script: `ConsoleBoardRS`,
`SensorCard2`, `GPSCard2`, `AudioCarrier`), the through-hole parts are left out too.

**Needs:** `kicad-cli`. The script uses `$KICAD_CLI` if set, else the macOS KiCad app's
`/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli` if it exists, else `kicad-cli` on the `PATH`.

```
python3 tools/fab_export.py [--placement-only] [BOARD ...]
```

With no board, it exports `DigiDash2` and `AudioCarrier`. `--placement-only` writes the placement, centroid and CPL
files and leaves the Gerbers alone.

```bash
python3 tools/fab_export.py SensorCard
python3 tools/fab_export.py --placement-only GPSCard2
KICAD_CLI=/usr/bin/kicad-cli python3 tools/fab_export.py RadioCard
```

Reads `<Board>/<Board>.kicad_pcb`. Writes:

| File | Content |
| --- | --- |
| `fab/gerbers/` | Gerbers (copper, paste, silkscreen, mask, edge cuts; inner layers when the board has them) and Excellon drill files with a Gerber X2 drill map. The folder is emptied first |
| `fab/<Board>_gerbers.zip` | The same files, zipped for upload |
| `fab/<Board>_placement.csv` | KiCad's position file (mm, both sides) |
| `fab/<Board>_centroid.csv` | PCBWay centroid: Designator, Mid X, Mid Y, Layer, Rotation |
| `fab/<Board>_CPL_JLCPCB.csv` | JLCPCB pick-and-place: KiCad's rotations normalised to 0–360 |

The PCBWay BOM (`fab/<Board>_BOM_PCBWay.csv`) is kept by hand and never written; the script only warns when its
designators no longer match the placed parts. JLC's reels don't always share KiCad's zero orientation, so check the
polarised parts in JLC's placement preview and add any fix to `JLC_ROT` in the script.

### mask_dams.py

Narrows fine-pitch SMD pads on a routed board so neighbouring pads are at least 0.22 mm apart, the smallest gap PCBWay
can hold a solder-mask dam in. Without a dam, the pads of a 0.5 mm QFP or 0.65 mm VSSOP share one mask opening and
bridge more easily.

For each footprint, pads of one size in a row are taken at the row's pitch; where the pitch is 0.8 mm or less and the
gap is under 0.22 mm, the pad width becomes pitch − 0.22 mm (length and centre unchanged). A pad is never narrowed by
more than a quarter of its width.

**When to use it:** after every regenerate of a board (`gen_pcb.py`), before `fab_export.py`. A regenerate loses the
change.

**Needs:** nothing beyond Python.

```
python3 tools/mask_dams.py [BOARD ...]
```

With no board it runs on `DigiDash2`, `GPSCard`, `AudioCarrier`, `SensorCard`, `RadioCard` and `ConsoleBoardRS`.

```bash
python3 tools/mask_dams.py SensorCard2
```

**Reads / writes:** edits `<Board>/<Board>.kicad_pcb` in place and prints, per footprint, how many pads changed. The
footprints then differ from the library copy, so KiCad's "footprint differs from library" warning is expected.

### smd_only.py

Writes the assembler's SMD-only files for a board into `<Board>/fab/assembly/`, with every through-hole line taken
out. The full files in `fab/` stay as they are (they are the shopping list).

**Needs:** `fab/<Board>_BOM_PCBWay.csv` with the columns `Item #`, `Designator`, `Qty` and `Type` (a `Type` starting
with `THT` marks a through-hole line). Run `fab_export.py` first so the placement files exist.

```
python3 tools/smd_only.py [BOARD ...]
```

With no board it runs on `SensorCard`, `RadioCard`, `GPSCard`, `AudioCarrier` and `ConsoleBoardRS`.

```bash
python3 tools/smd_only.py SensorCard RadioCard
```

Writes `fab/assembly/<Board>_BOM_SMD.csv` (renumbered) and, for each of `_CPL_JLCPCB.csv`, `_placement.csv` and
`_centroid.csv` that exists, the same file with an `_SMD` suffix. Prints the SMD line and part counts and the
through-hole designators left to you.
