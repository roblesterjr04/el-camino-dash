# DigiDash main board, rev 3: all through-hole

Rev 2 ([`DigiDash2/`](../DigiDash2/)) with every SMD part swapped for a through-hole one, so the board needs **no
assembly service**: order the bare PCB and solder it by hand (about 170 parts, all leaded). Same outline, slot row,
screen headers, mounting and screen holes and rear connector group as rev 2, so the housing fits both. Started
2026-10-26 to get the first set under the fab's assembly fees and the 35 % US duty on assembled boards.

| File | What |
| --- | --- |
| `DigiDash3.kicad_sch` / `.kicad_pcb` / `.kicad_pro` | The board: 329 × 113 mm, **2 layers**, parts on the back, screen headers on the front. KiCad 10 |
| `DigiDash3.pretty/` | The slot socket (numbered to mate with the cards) and a radial PTC footprint, from `generate/gen_footprints.py` |
| `DigiDash3_BOM.csv` | Parts list for hand assembly (`generate/gen_bom.py`): grouped by vendor (DigiKey, then LCSC), with each line's product link and its unit price at quantity 1 where the page showed one (looked up 2026-10-26; `SOURCE` in the generator) |
| `fab/` | Gerbers, drill, placement files (`python3 ../tools/fab_export.py DigiDash3`) |
| `generate/` | The generators, from rev 2's: `gen_sch.py` (schematic), `gen_pcb.py` (placement + Freerouting), `gen_bom.py`, `drc.py` |

## What changed from rev 2

| Block | Rev 2 (SMD) | Rev 3 (through-hole) |
| --- | --- | --- |
| 5 V | One TPS54560B, 5 A, 400 kHz | **Two LM2576HV-5.0** (TO-220-5 lying flat, 3 A, 52 kHz): U2 feeds the three screens (`+5V_SCR`), U3 feeds the board, the six slots and the cable ports (`+5V`). Each: 100 µF 63 V in, SB560 catch diode, **68 µH** radial inductor (Abracon AIAP-03-680K, 3.3 A: no stocked 100 µH part of 3 A or more fits a 14 mm / 10 mm-pitch footprint, and 68 µH costs about 45 % more ripple current at this load, still continuous), 1000 µF 16 V out (TI SNVS107 figure 1) |
| Enable | EN divider from the ignition, WAKE through a BAT54S | Both bucks' ON/OFF pins on one 2N3904 (Q7): low (on) while the ignition is on (47k) or a card holds WAKE (1N4148 + 10k). Off, the pins rest at 12V_BATT through 100k (ON/OFF may go to VIN) |
| 3.3 V | AP7361C-33 SOT-223 | LD1117V33, TO-220 lying flat, 10 µF electrolytics in and out. Also feeds the video module when JP1 is strapped to 3.3 V |
| 1.8 V | AP2112K-1.8 for the TVP5150 core | Gone with the decoder |
| Reverse / switch FETs | SUD50P04 DPAK ×3 | IRF4905 TO-220 lying flat ×3 (55 V, 20 mΩ), gates clamped by 1N4744A (15 V) |
| TVS | SMCJ33A ×2 | 1.5KE33A (DO-201) ×2 |
| CAN transceivers | MCP2562-E/SN SOIC ×3 | **MCP2562-E/P DIP-8** ×3 (same pins) |
| CAN ESD clamps | NUP2105L SOT-23 ×2 | Two SA24CA bidirectional TVS (DO-15) per site: CANH and CANL to ground |
| Clamps / wake diode | BAT54S ×3 | BAT85 (DO-35) ×4, 1N4148 ×1 |
| PTCs | 1812 / 1206 SMD ×35 | Bourns MF-R radial ×29 (MF-R050, R110, R010, R020; same hold currents, 60 V except the 1.1 A at 30 V) |
| Video | TVP5150AM1 on the board, crystal, RCA input chain | **J8: a 2×10 socket for a TVP5150 breakout module**, JP1 (its VCC: 5 V or 3.3 V), J10 (PCLK / RESET wires). The module carries its own video input, crystal and regulators, so J11 (RCA) and the input chain are gone |
| Termination switch | KingTek SMD | Through-hole 2-way slide DIP (DSWB02LHGET) |
| Passives | 0603 / 0805 / 1210 | 1/4 W axial resistors (7.62 mm), 5 mm disc 100 nF, radial electrolytics |
| Layers | 4 (GND and +5 V planes) | **2**, ground pours on both sides; 0.25 mm tracks, 0.15 mm clearance, 0.6 / 0.3 vias (JLCPCB's 2-layer limit is 0.127 / 0.127); the 5 V feeds at 1.5 mm, 12 V at 1.0 mm. The slot sockets' pads are 1.5 mm on 1.0 mm holes (rev 2: 1.7), so two tracks pass between neighbours: at 0.3 / 0.2 and 1.7 mm pads the router left bus nets open |

Unchanged: J7 / J13 / J9 / J6 / J16 / J27 / J17–J25 and their pin use, the six slot sockets and their pinout
([`accessory_ports.md`](../docs/hardware/accessory_ports.md)), the AUX 12 V path, the sensor-serial fan-out, the
illumination / reverse inputs, the slot ID resistors, WAKE, LAMP / REV levels, the audio bus and I2S rail, and the
screen headers' pin use (`HDR_NETS` in `gen_sch.py`, with `+5V_SCR` on pins 1 and 3).

## The video module socket (J8)

J8's pin order is the module's own, odd pins in one row, even in the other, 1–20:

| Pin | Net | Pin | Net |
| --- | --- | --- | --- |
| 1 | VID_VCC (JP1: 5 V or 3.3 V) | 2 | GND |
| 3 | CL_SCL (cluster I2C) | 4 | CL_SDA |
| 5 | V_VS → J1 pin 24 | 6 | V_HS → J1 pin 30 |
| 7 | n.c. | 8 | n.c. |
| 9 | V_D7 → J1 pin 16 | 10 | V_D6 → J1 pin 32 |
| 11 | V_D5 → J1 pin 35 | 12 | V_D4 → J1 pin 39 |
| 13 | V_D3 → J1 pin 8 | 14 | V_D2 → J1 pin 11 |
| 15 | V_D1 → J1 pin 12 | 16 | V_D0 → J1 pin 14 |
| 17 | n.c. | 18 | V_FID → J1 pin 37 |
| 19 | INT (10k to ground, unused) | 20 | ID (10k to ground, **not fitted**: only if the module wants its address strap here) |

**The module's header has no pixel-clock pin.** The center screen's camera interface needs PCLK (J1 pin 25), so J10
pin 1 carries that net: run a wire from the module's PCLK point (a test pad or the decoder's SCLK pin) to J10 pin 1.
J10 pin 2 is the center screen's reset line (GPIO52, J1 pin 36) for the module's RESET if it has one; pin 3 is GND.
Check the module's supply before strapping JP1: most TVP5150 breakouts take 3.3 V. Fit a **female** 2×10 socket at
J8 (the module carries the pins; the BOM says so).

## Board

- **Sides:** every part's body is on the back; only the three screen headers are on the front. Soldering is from
  the front (the pin tails come through there), so the screens come off first.
- **Heights on the back, outside the cards:** rev 2's housing allowed about 10.5 mm over the power section. Rev 3's
  power parts are taller: the inductors (Abracon AIAP-03-680K, 11.4 mm across) stand **23 mm**, the tallest parts on the board; the 1000 µF output caps (Rubycon 16YXJ1000MT810X16) 16 mm;
  the 100 µF caps 12.5 mm; the radial PTCs 8–9 mm; the TO-220s lying flat about 5 mm; the DIP transceivers 4 mm. The
  video module on J8 stands about 12 mm plus its own height. **The housing's power-section step needs to grow to
  about 25 mm over the two inductors (18 mm elsewhere)**, and the module's footprint (about 40 × 30 mm, at x 190–220, y 60–80) needs room behind J8.
- **Under the cards** (7 mm limit) only lying-flat resistors, diodes, DIPs and TO-92s go; the placer keeps the tall
  parts out of the card envelopes (`TALL_RE` in `gen_pcb.py`).
- **Placement:** slots, screen headers, holes and the rear connector group as rev 2 (`SLOTS`, `SCREENS`, `HOLES_MH`,
  `GROUP`). The two bucks lie flat at x 170, y 44 and 60, pins toward the slots; Q5 and the LDO east of them; the
  video socket below the power section at (205, 69); the transceivers beside their headers.

## Status

**Schematic: done** (168 parts, 2026-10-26). **PCB: placed, routed and DRC-clean** (2026-10-26): Freerouting at
150 passes, 2083 tracks and vias, then **two links drawn by hand** that the finisher would not make because they end
on the net ties' pads (its clearance check keeps everything off a tie): AUDIO_GND from its F.Cu run at y 63.42 up to
NT2 pad 2 (91.75, 61.85), and AUX_GND on B.Cu, 1.0 mm, from (249.65, 64.0) down to NT1 pad 2 (249.65, 71.5).
**A regeneration drops those two** (re-add them, or check the DRC's two unconnected items). DRC: 0 errors; the
expected warnings are the two library-mismatch notes and two silkscreen overlaps. Fab files exported. The inductors' leads are bent out to the 10 mm pads (the Abracon's own pitch is narrower); the body sits
above the pads. Not yet checked: the LM2576 layout by eye (each buck's input cap,
catch diode and inductor should sit tight round its pins), the housing against the heights above, and the module's
PCLK / RESET / VCC against the actual breakout.

## Regenerating

KiCad 10, numpy in KiCad's python, Freerouting 1.9.0 (the jar in `../DigiDash2/generate/`) on Java 17+:

```bash
KIPY=/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3
python3 generate/gen_footprints.py && python3 generate/gen_sch.py   # also writes generate/refs.json
JAVA=/opt/homebrew/opt/openjdk@17/bin/java FR_PASSES=60 $KIPY generate/gen_pcb.py --route ../DigiDash2/generate/freerouting-1.9.0.jar
python3 generate/drc.py && $KIPY generate/gen_bom.py && python3 ../tools/fab_export.py DigiDash3
```

With no planes there is no fan-out or stitching step: Freerouting routes GND like any other net, and the ground pours
are filled afterwards. `--from-ses`, `--finish`, `--fill` and `--labels` work as on rev 2. Do not set `DIGIDASH_RIPUP=1`
on this board: the rip-up finisher cascaded for over an hour without ending (2026-10-26).
