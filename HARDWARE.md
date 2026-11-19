# Hardware

Every board in this repository, which revision replaces which, and how the boards are generated and sent to a fab.
Each board directory has its own README with the details; this page only collects them.

Status words below: **designed** means the schematic and board files exist and are routed; **ordered**, **built**
and **tested** are used only where a README or the git history says so. "Not stated" means nothing in the repository
says either way.

## Lineage

```
Main board   DigiDash (rev 1, through-hole, plug-in modules)
               └─► DigiDash2 (rev 2, SMD, 4 layers, card slots)
                     └─► DigiDash3 (rev 3, rev 2 in through-hole, 2 layers)
             BenchBoard: a small stand-in for DigiDash2 on the bench

Sensor       DigiDash rev 1's on-board ATmega328P sensor pack
               └─► SensorCard (rev A, ATmega2560 + MCP2515)
                     └─► SensorCard2 (rev B, STM32G0B1)

GPS          GPSCard (rev A, STM32G0B1, SMD)  ──  GPSCard2 (rev B, PIC18F26Q83, through-hole)

Console      ConsoleBoard (keys and encoders on the board)
               └─► ConsoleBoard4Key (four lit keys; renamed the same day)
                     └─► ConsoleBoardRS (RetroSound knobs on RJ9)

Others       AudioCarrier (audio card), RadioCard, LTECard; Panel (fab panel of the current boards); Housing
```

## Boards

| Directory | What it is | Supersedes / superseded by | Size, layers | MCU | Firmware | Status |
| --- | --- | --- | --- | --- | --- | --- |
| [`DigiDash`](DigiDash/README.md) | Main board, rev 1: the three screen headers, an ATmega328P sensor pack, plug-in FRAM and video modules, four accessory ports | Superseded by DigiDash2. Its `DigiDash_Accessory.pretty` is still in the other boards' library tables | 309.2 × 103.7 mm, 2 layers | ATmega328P (DIP) | [`arduino/sketches/sensor_pack`](arduino/sketches/sensor_pack/) | Designed. Ordered / built: not stated |
| [`DigiDash2`](DigiDash2/README.md) | Main board, rev 2: screen headers, six card slots, 5 V buck, TVP5150 video decoder, cable and trunk ports. Every SMD part on the back | Supersedes DigiDash | 329 × 113 mm, 4 layers | None (the screens are ESP32-P4 boards) | Screens: [`firmware/esp32_p4_display`](firmware/esp32_p4_display/) | Designed: fully routed, DRC clean (2026-10-20). Firmware steps in its README not done. Ordered: not stated |
| [`DigiDash3`](DigiDash3/README.md) | Main board, rev 3: rev 2 with every part through-hole, for hand assembly; same outline and mounting | Derived from DigiDash2; both kept | 329 × 113 mm, 2 layers | None | As DigiDash2 | Designed: routed, DRC 0 errors (2026-10-26), two hand-drawn links. Ordered: not stated |
| [`BenchBoard`](BenchBoard/README.md) | Bench stand-in for DigiDash2: two card slots, one screen header, 12 V in, a USB-CAN header | — | 100 × 100 mm, 2 layers | None | Whatever plugs in | Designed (2026-10-05). Nothing ordered (its README) |
| [`SensorCard`](SensorCard/README.md) | Sensor card, rev A: senders, speed, tach, ignition, battery, odometer in FRAM, on CAN as node `0x04` | Replaces rev 1's on-board sensor pack | 80 × 60 mm, 4 layers | ATmega2560 + MCP2515 | [`firmware/sensor_card`](firmware/sensor_card/README.md) (host-tested, not run on a card) | Designed: routed, DRC clean. On the panel. Ordered: not stated |
| [`SensorCard2`](SensorCard2/README.md) | Sensor card, rev B: rev A on an STM32G0B1 with built-in FDCAN; same connectors, frames and FRAM layout | Supersedes SensorCard (started 2026-10-26) | 80 × 60 mm, 2 layers | STM32G0B1KBU6 | [`firmware/sensor_card_stm32`](firmware/sensor_card_stm32/README.md) (host-tested, not run on a card) | Designed: PCB generated; oscillator fix 2026-11-13. Not built (its README) |
| [`AudioCarrier`](AudioCarrier/README.md) | Audio card: Bluetooth sink, two PCM5102A DACs, PCM1808 ADC with mic preamp, RCA outputs; I2S rail clock master. Goes in slot 1 | Rev J (2026-11-19) replaces the module version (rev I and earlier, in git) | 80 × 60 mm, 2 layers (4 up to rev I) | ESP32-DevKitC-VE (ESP32-WROVER-E) in sockets | [`firmware/esp32_classic_bt_audio`](firmware/esp32_classic_bt_audio/) (builds `arduino/sketches/esp32_audio_bridge`) | Designed: rev J routed, DRC 0 errors. Firmware runs on a bench dev kit. Card ordered / built: not stated |
| [`RadioCard`](RadioCard/README.md) | AM/FM radio card: Si4735 tuner on the I2S rail, SMA antenna jack | — | 80 × 60 mm, 2 layers | STM32G0B1KBU6 | Not written; `tools/radio_sim.py` stands in | Designed: routed, DRC clean; oscillator fix 2026-11-13. Ordered: not stated |
| [`GPSCard`](GPSCard/README.md) | GPS card, rev A: reads a remote GPS puck, sends position and time on CAN | — | 80 × 60 mm, 2 layers | STM32G0B1KBU6 | Not written | Designed: routed, DRC clean; oscillator fix 2026-11-13. Ordered: not stated |
| [`GPSCard2`](GPSCard2/README.md) | GPS card, rev B: the same card with every part through-hole | Alternative to GPSCard, same slot and messages | 80 × 60 mm, 2 layers | PIC18F26Q83 (SPDIP-28) | Not written | Designed: routed, DRC clean. Not ordered, not built (its README) |
| [`LTECard`](LTECard/README.md) | LTE gateway card: ESP32-S3 with a SIMCom A7670 core board, online data on CAN | — | 80 × 60 mm, 4 layers | ESP32-S3-WROOM-1-N16R8 | Not written; plan in [`docs/hardware/lte_card.md`](docs/hardware/lte_card.md) | Designed: routed, DRC clean (2026-10-01). Not ordered (its README); core board geometry provisional |
| [`ConsoleBoard`](ConsoleBoard/README.md) | Console: ESP32-S3, SSD1322 OLED, Back / View keys, two encoders | Superseded by ConsoleBoardRS (left off the panel) | 100 × 50 mm, 4 layers | ESP32-S3-WROOM-1-N8 | [`firmware/esp32s3_console`](firmware/esp32s3_console/README.md) (now written for ConsoleBoardRS) | Designed. Ordered: not stated |
| [`ConsoleBoard4Key`](ConsoleBoard4Key/README.md) | Console with four lit keys, for a 111.5 × 38.5 mm housing. Only its `.kicad_pro` is tracked | Became ConsoleBoardRS (2026-10-03) | 108 × 35 mm (git history) | ESP32-S3 | — | Abandoned |
| [`ConsoleBoardRS`](ConsoleBoardRS/README.md) | Console: ESP32-S3, SSD1322 OLED, two RetroSound knobs on RJ9, IR receiver | Supersedes ConsoleBoard and ConsoleBoard4Key | 108 × 35 mm, 2 layers (4 before 2026-10-26) | ESP32-S3 | [`firmware/esp32s3_console`](firmware/esp32s3_console/README.md) | Designed. On the panel. Ordered: not stated |
| [`Panel`](Panel/README.md) | DigiDash2, GPSCard, AudioCarrier, SensorCard, RadioCard and ConsoleBoardRS on one fab panel (KiKit) | — | 329 × 255.5 mm, 4 layers | — | — | Generated; its files predate AudioCarrier rev I and J. Ordered: not stated |
| [`Housing`](Housing/README.md) | Onshape FeatureScript for the cluster housing; a printed turn-signal LED puck | — | — | — | — | Model v6 (2026-11-17), collision-checked in CAD. Printed: not stated |

Off-the-shelf boards the dash uses: Waveshare ESP32-P4 round display boards (4" 720×720 left and right, 3.4"
800×800 in the center) on the main board's 2×20 headers, an ESP32-DevKitC-VE on the audio card, a SIMCom A7670 core
board on the LTE card and a TVP5150 breakout on DigiDash3.

## Working with the boards

### Tools

- **KiCad 10.** The board files are KiCad 10 format; older KiCad versions cannot open them. Footprints are from
  KiCad's stock libraries except the few in each board's `*.pretty` folder (listed in its README) and
  `DigiDash/DigiDash_Accessory.pretty`.
- **Python.** The generators run on the system Python 3 (schematic, DRC wrapper) and on KiCad's bundled Python
  (`$KIPY`, for anything that uses `pcbnew`), with numpy installed into KiCad's Python:
  `$KIPY -m pip install --user numpy`. The READMEs give the macOS path of `$KIPY`; use your platform's.
- **Freerouting 1.9.0** on Java 17 or newer. The jar is not in the repository (`freerouting-*.jar` is gitignored):
  download it from the Freerouting GitHub releases into the board's `generate/` folder, or point `--route` at one
  copy. It opens its window while it routes.
- **KiKit 1.8.1** for the panel only, in a venv on KiCad's Python (see [`Panel/README.md`](Panel/README.md)).

### Generated, not drawn

Each board is produced by the scripts in its `generate/` folder. Edit the scripts and re-run them; edits made in
KiCad's editors are lost on the next run. The usual set:

| Script | Does |
| --- | --- |
| `gen_footprints.py` | Writes the board's own footprints into its `*.pretty` folder (only on boards that have one) |
| `gen_sch.py` | Writes the schematic and `generate/refs.json` (part groups, not-fitted flags) |
| `gen_pcb.py` | Places the parts, then with `--route` runs Freerouting once, joins leftover pieces of a net, adds ground vias and stitching, and fills the zones |
| `drc.py` | KiCad DRC with the schematic parity check; reports in `$TMPDIR` |
| `gen_bom.py` | The BOM CSV and, on some boards, the assembler's BOM (only some boards have it) |
| `sexp.py` | Shared S-expression helpers |

A typical run, from the board's directory (each README has the exact one):

```bash
KIPY=/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3
python3 generate/gen_footprints.py                 # if the board has one
python3 generate/gen_sch.py
kicad-cli sch erc <Board>.kicad_sch                 # expect warnings only (off-grid pins, library nickname)
JAVA=/path/to/java17 $KIPY generate/gen_pcb.py --route freerouting-1.9.0.jar
python3 generate/drc.py
```

`gen_pcb.py` options that most boards share: `--from-ses` (redo the steps after Freerouting from its last session),
`--stitch` and `--fill` (redo only those steps), `FR_PASSES` (Freerouting's pass limit) and `FR_TRIES` (re-run it from
the placed board). Freerouting's result changes with any placement change, so a re-run can leave a different link
open; the READMEs say what worked last time.

**Hand edits that a regeneration drops.** Some routed boards were finished by hand after the generator, and those
fixes are not in the scripts: DigiDash3's two net-tie links, the AudioCarrier's via nudges (PCBWay's 0.15 mm
via-to-pad rule) and its one-off routes from rev I, and the in-place oscillator edits on GPSCard and RadioCard. Read
the board's Status section before regenerating it.

### After regenerating

1. **Mask dams.** `python3 tools/mask_dams.py <Board>` narrows fine-pitch SMD pads so neighbours stay at least
   0.22 mm apart (PCBWay's smallest solder-mask dam). It edits the `.kicad_pcb` in place, so run it again after every
   regeneration, then refill the zones (`gen_pcb.py --fill`). KiCad then warns that those footprints differ from the
   library; that is expected.
2. **DRC.** `python3 generate/drc.py` again. Each board's README lists the warnings it expects.
3. **Fab outputs.** `python3 tools/fab_export.py <Board>` (from the repository root, or `../tools/fab_export.py`
   from the board's directory).
4. **The panel**, if the board is on it: see [`Panel/README.md`](Panel/README.md#regenerating).

### The `fab/` folder

Written by `tools/fab_export.py` unless noted:

| File | What |
| --- | --- |
| `fab/gerbers/`, `fab/<Board>_gerbers.zip` | Gerbers and drill files, zipped for upload |
| `fab/<Board>_placement.csv`, `fab/<Board>_centroid.csv` | Placement (pick-and-place) files, from the board's bottom-left corner. Not-fitted parts are left out, and on boards whose through-hole parts are soldered by hand, those too |
| `fab/<Board>_CPL_JLCPCB.csv` | The JLCPCB CPL file. Check polarised parts in JLC's placement preview; rotation fixes go in `JLC_ROT` in the script |
| `fab/<Board>_BOM_PCBWay.csv` | The assembler's BOM. Kept by hand or written by the board's `gen_bom.py`; `fab_export.py` only warns when it no longer matches the board |
| `fab/assembly/` | SMD-only copies of the BOM and placement files, for an order where you solder the through-hole parts (`python3 tools/smd_only.py <Board>`) |
| `fab/pcbway/`, `fab/protoexpress/` | Per-vendor upload sets (Gerbers, centroid, BOM) on some cards |
| `fab/*.png`, `fab/*.pdf` | Renders and assembly drawings, where a board has them |

`<Board>_BOM.csv` in the board's own directory is the full parts list with maker and part number.

### Ordering

- [`docs/hardware/panel_order_costing.md`](docs/hardware/panel_order_costing.md) breaks down a quote: parts, assembly
  fees, US duties, and cheaper scenarios (JLCPCB, PCBWay, the through-hole DigiDash3).
- `Panel/fab/Panel_BOM_resourced.csv` is the parts list re-sourced to stocked parts, with a source per line.
- [`docs/hardware/cable_harnesses.md`](docs/hardware/cable_harnesses.md) lists the mating cables to order with the
  boards.
- Each board README has a "Check before ordering" list. Do those first.

## License

The hardware designs (KiCad files, footprints, fab outputs, BOMs, `Housing/`) are licensed under the CERN Open
Hardware Licence v2, Permissive ([`LICENSE-HARDWARE`](LICENSE-HARDWARE)). The Python generator scripts in each
`generate/` folder are software, under MIT ([`LICENSE`](LICENSE)). 3D models embedded in the `boardmodel.step` /
`.stl` exports come from the KiCad library (CC-BY-SA 4.0 with the KiCad exception).
