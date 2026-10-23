# Panel

All the current boards on **one 4-layer, 1.6 mm panel, 329 × 253 mm**, broken apart along mouse-bite tabs
(0.5 mm holes at 0.8 mm pitch, 5 mm tabs, 2.5 mm mill gaps), with 5 mm rails top and bottom that hold four
1.152 mm tooling holes, three fiducials on each side, and the panel name.

```
+-------------------------------------- DigiDash2 329 x 113 ----------------------------------------+
+-- GPS 80x60 --+ +-- Audio 80x60 --+   +-- Sensor 80x60 --+        (air for the SMA)
+---------------+ +-----------------+   +------------------+              ^ SMA
                              +---- ConsoleBoardRS 108x35 ----+    +-- Radio 80x60 --+
```

Two gaps are 5 mm instead of 2.5 mm (Audio | Sensor and Console | Radio): the Sensor and Radio cards' left-edge
headers stand 6.5 mm past the edge, and at 2.5 mm they would sit on the Audio card's 3.5 mm jack and the console's
OLED socket. At 5 mm they clear by about 1.8 mm.

| Prefix | Board |
| --- | --- |
| DD | `DigiDash2` |
| GP | `GPSCard` (2-layer board: goes in with empty inner layers) |
| AU | `AudioCarrier` |
| SE | `SensorCard` |
| RA | `RadioCard` (2-layer board: goes in with empty inner layers) |
| CO | `ConsoleBoardRS` |

Left out: `DigiDash` (rev 1, reference only), `ConsoleBoard` (replaced by ConsoleBoardRS), `LTECard`, `BenchBoard`.

References and nets carry the board's prefix (`AU-R1`, `AU-GND`); the silkscreen keeps the board's own (`R1`).

## Through-hole parts are assembled too

Since 2026-10-23 the assembler fits **every** part, through-hole included: all 49 connectors, headers, slot sockets,
DigiDash2's fuse holder F1 and the console's IR receiver are in the BOM (type THT) and the placement files. Only a
board's own not-fitted parts (DigiDash2's C42 / C43, the Radio card's R13) stay out. Pads-only footprints (solder
jumpers, mounting holes, net ties) are out of the BOM and placement files.

Through-hole parts that hang past their board edge (the cards' right-angle headers, the SMA, the RCA block) reach
over a gap, a rail, empty air, or a neighbour's bare edge, never a neighbour's part on the same side; `gen_panel.py`
does not check this, so re-check it with a courtyard overlap test after moving a board. Tabs keep clear of SMD
overhangs only (antennas, USB-C, SMA): a header's pins may cross a tab and the board breaks out clear of them. Snap
the cards out with care, pins first.

## Tabs

The generator spaces tabs about one per 45 mm of shared edge, with none within 1.5 mm of a placed part that hangs
past its board edge (the Audio and console ESP32 antennas, `CO-J2`, `RA-J2`). Gaps with only one board facing a
rail run the tab all the way to the rail. It checks that the panel is one piece and that every gap stays mostly open.

## Files

| Files | |
| --- | --- |
| `Panel.kicad_pcb` / `.kicad_pro` | The panel. Generated: do not edit, re-run the generator |
| `generate/gen_panel.py` | Builds the panel with [KiKit](https://github.com/yaqwsx/KiKit). Layout, gaps, tabs: constants at the top |
| `generate/gen_bom.py` | `fab/Panel_BOM_PCBWay.csv` from the placement file plus each board's own PCBWay BOM (DigiDash2's from `DigiDash2/generate/gen_bom.py`) |
| `fab/` | Gerbers + drill, placement / centroid / JLC CPL, PCBWay BOM |

## Regenerating

KiKit runs on KiCad's own Python. Make the venv once (`.venv` is gitignored):

```bash
/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3 -m venv --system-site-packages Panel/.venv
Panel/.venv/bin/pip install kikit==1.8.1
```

Then, from the repo root, after any change to a board:

```bash
Panel/.venv/bin/python Panel/generate/gen_panel.py && python3 tools/fab_export.py Panel && python3 Panel/generate/gen_bom.py
```

## Open items

- **Panel DRC is noisy, but not from geometry.** The panel uses DigiDash2's design rules (0.2 mm clearance); the
  cards were drawn to tighter rules, so their own copper shows ~500 clearance errors and the Audio card's antenna
  keep-out shows 90. Each card is DRC-clean in its own project. Zone fills are copied, not refilled, so the copper
  is the boards' own. Check each board in its own project, not the panel.
- The right-angle headers on the card edges overhang into the gaps and rails and are now fitted on the panel. Ask
  the fab to break the boards out by hand, not with a router or V-score tool that runs along those edges. Sand the
  mouse-bite nubs flat where a header or card edge must seat in a slot.
- `AU-J6` (Tinkersphere TS3328 3.5 mm jack) is not a distributor part: the fab may ask for it to be supplied. Its pin
  functions are still to be checked with a meter (see `AudioCarrier/README.md`).
- Mixed boards on one panel: confirm with the fab that they accept a multi-design panel (it often counts as
  several designs).
