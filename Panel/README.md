# Panel

All the current boards on **one 4-layer, 1.6 mm panel, 329 × 253 mm**, broken apart along mouse-bite tabs
(0.5 mm holes at 0.8 mm pitch, 5 mm tabs, 2.5 mm mill gaps), with 5 mm rails top and bottom that hold four
1.152 mm tooling holes, three fiducials on each side, and the panel name.

```
+-------------------------------------- DigiDash2 329 x 113 ----------------------------------------+
+-- GPS 80x60 --+ +-- Audio 80x60 --+ +-- Sensor 80x60 --+          (air for the SMA)
+---------------+ +-----------------+ +------------------+                ^ SMA
                            +---- ConsoleBoardRS 108x35 ----+  +-- Radio 80x60 --+
```

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

## Through-hole parts are not placed

Every through-hole footprint (49: connectors, headers, and DigiDash2's fuse holder F1) is marked **DNP**,
excluded from the BOM and excluded from the placement files: they are hand-soldered after the boards are broken
out. Pads-only footprints (solder jumpers, mounting holes, net ties) are out of the BOM and placement files too.
Footprints with SMD attributes and plated shell pegs stay in: the USB-C `CO-J2` and the edge-mount SMA `RA-J2`.

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
- The right-angle headers on the card edges overhang into the gaps and rails; they are hand-soldered after
  break-out, so this does not matter for assembly. Sand the mouse-bite nubs flat where a header or card edge must
  seat in a slot.
- Mixed boards on one panel: confirm with the fab that they accept a multi-design panel (it often counts as
  several designs).
