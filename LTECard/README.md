# LTE gateway card

A **slot card** for the DigiDash rev 2 board: **80 × 60 mm**, standing on edge in a 2×15 slot, its right-angle header
on the bottom edge (`docs/hardware/accessory_ports.md`, rev 2 section). Spec: [`docs/hardware/lte_card.md`](../docs/hardware/lte_card.md).
It goes in **any slot**. On it:
- an **ESP32-S3-WROOM-1-N16R8** module (16 MB flash, 8 MB octal PSRAM), antenna at the top-left corner
- a **SIMCom A7670 LTE Cat-1 core board** (a ready-made breakout) on two female sockets, 1 × 12 and 1 × 11, 8.5 mm tall
- a **TPS54560B buck** making the core board's 5 V from **AUX 12 V** (slot pins 25–30), with 470 µF bulk at its VIN
- an **MCP2562** CAN transceiver and an **AP7361C-33** 3.3 V regulator, both from the slot's 5 V
- **USB-C** (mid-mount, left edge) to the ESP32-S3's native USB, **BOOT** and **RESET** buttons, a **debug serial**
  header, two LEDs

CAN node from its slot (`0x14 + (slot − 1)`). Firmware: `docs/hardware/lte_card.md`, "Firmware".

> ## Measure the core board before ordering
>
> The core board's geometry and pin order on this card are **provisional**: they come from the vendor listings, not
> from the board in hand. Check every item, edit the constants, re-run the generators (see [Regenerating](#regenerating)):
>
> 1. **Pin order of both rows.** `CORE_ROW_A` (12 pins, left socket J2) and `CORE_ROW_B` (11 pins, right socket J3) at
>    the top of `generate/gen_sch.py`. The current order is a guess that groups power / control on row A and the UART on
>    row B. The names are the board's silkscreen names; `CORE_NETS` maps them to the card.
> 2. **Which end is pin 1**, and the pin counts per row (12 / 11?): `CORE_PIN1_AT_BOTTOM`, `CORE_ROW_A_N`, `CORE_ROW_B_N`
>    in `generate/gen_pcb.py`. Now: pin 1 of both rows at the bottom (toward the slot), so VIN sits by the 470 µF.
> 3. **Row-to-row spacing**, centre to centre: `CORE_ROW_PITCH` (now 40.64 mm = 16 × 2.54), and whether each row is
>    centred on the board's height (`CORE_ROW_A_DY`, `CORE_ROW_B_DY`).
> 4. **Board outline**: `CORE_W` × `CORE_H` (now 44.45 × 31.75 mm), at `CORE_X0`, `CORE_Y0` (upper right). Nothing
>    is placed under it on the parts side. Check the height: 8.5 mm sockets + 1.6 mm + its tallest part must stay under
>    the 20 mm parts-side limit, and the A7670 and IPEX plug underneath must clear the card (8.5 mm).
> 5. **PEN and RST**: polarity, and whether the board pulls them up (and to what voltage). The card drives PEN through
>    1k with a 100k pull-down (LTE off until the firmware raises it) and RST through 1k.
> 6. **STA and NET levels** (1.8 V from VEXT, 3.3 V from the level shifter, or open-collector LED drive?). They come in
>    through 1k each. If they are 1.8 V, the ESP32 (VIH about 2.5 V) will not read them: add a level shifter or move them
>    to an ADC pin.
> 7. **RTS / CTS direction**: wired the SIMCom way (the module's RTS is an input, the ESP32's RTS output drives it).
> 8. **Where the IPEX antenna socket is**, and that the pigtail reaches the case panel.

| Files | |
| --- | --- |
| `LTECard.kicad_sch` / `.kicad_pcb` / `.kicad_pro` | The card: 80 × 60 mm, 4 layers, parts on the front (two capacitors on the back). KiCad 10 |
| `LTECard_BOM.csv` | Bill of materials |
| `generate/` | Scripts that produced the files (`gen_sch.py`, `gen_pcb.py`, `drc.py`, `sexp.py`; `refs.json` is written by `gen_sch.py`). Copied from `AudioCarrier/generate/` and adapted. Every footprint is stock KiCad 10, so there is no `gen_footprints.py` or `.pretty` library |

## What's on it

```
AUX 12 V (pins 25-27) ─ 3 × 2.2 µF 100 V ─ TPS54560B (400 kHz) ─ 6.8 µH ─ LTE_5V ─┬─ 3 × 47 µF
AUX GND  (pins 28-30) ─────── all of the buck's grounds, the core board's GND ─┘    └─ 470 µF ─ core board VIN × 2
                                                                LTE_5V ─ 22k / 33k ─ GPIO5 (AUX present)
+5 V (pins 1, 2) ─ AP7361C-33 ─ +3V3 ─ ESP32-S3, MCP2562 VIO, core board VTTL (its TXS0108E reference)
ESP32-S3 UART1 TX GPIO41 ──► core RXD     RX GPIO42 ◄── core TXD     RTS GPIO40 ──► core RTS    CTS GPIO39 ◄── core CTS
         DTR GPIO38 ──► core DTR          RI GPIO21 ◄── core RI      (no pull-ups: the TXS0108E does not like them)
         PEN GPIO11 ─ 1k ─► core PEN (100k pull-down)   RST GPIO12 ─ 1k ─► core RST
         STA GPIO13 ◄─ 1k ─ core STA                    NET GPIO14 ◄─ 1k ─ core NET
CAN: GPIO9 TX / GPIO10 RX ── MCP2562 (VDD 5 V, VIO 3.3 V, STBY low, no termination) ── pins 5 / 6
Slot ID (pin 11): 10k to 3.3 V here, the slot's resistor to GND on the main board ──► GPIO4 (ADC1)
USB-C D+ / D- ── USBLC6-2SC6 ── GPIO20 / GPIO19 (native USB); CC1 / CC2 5.1k to GND; VBUS only to the clamp
```

### ESP32-S3 pins

| GPIO | Function | GPIO | Function |
| --- | --- | --- | --- |
| 41 | UART1 TX → core RXD | 9 | CAN TX (TWAI) |
| 42 | UART1 RX ← core TXD | 10 | CAN RX (through 1k) |
| 40 | UART1 RTS → core RTS | 4 | Slot ID (ADC1_CH3) |
| 39 | UART1 CTS ← core CTS | 5 | AUX present (ADC1_CH4): 3.0 V with the buck running |
| 38 | DTR out (hold low) | 1 | LED D1, LTE |
| 21 | RI in | 2 | LED D2, Wi-Fi |
| 11 | PEN out (1k, 100k pull-down) | 19 / 20 | USB D− / D+ |
| 12 | RST out (1k) | 43 / 44 | TXD0 / RXD0, debug serial J5 |
| 13 | STA in (1k) | 0 | BOOT button (strap) |
| 14 | NET in (1k) | EN | RESET button, 10k / 1 µF |

Not used: the strapping pins GPIO3, 45, 46 (and GPIO0 except for BOOT), the octal-PSRAM pins GPIO35–37, GPIO47 / 48.
UART1 runs through the GPIO matrix (its default pins are the USB pins).

### Grounds: AUX GND is not GND

On the main board AUX GND meets GND **only at NT1**, at the power entry. If this card joined them too, the two would
form a loop through NT1, and AUX return current (this card's and every other card's) would share the slot GND pins and
the main board's logic ground. So **the card does not join them**: there is no net tie.
- **AUX side (AUX_GND):** the buck and everything around it, the 470 µF, and the core board's GND pins. AUX_GND has
  its own pours on F, In2 and B over the buck area (stitched with AUX_GND vias), 0.8 mm tracks elsewhere.
- **Logic side (GND):** the ESP32, CAN, USB, LDO. In1 is a solid GND plane under the whole card, the buck included
  (it is only a reference plane there: nothing of the AUX side connects to it).
- Only logic signals cross between the two (UART, control lines through 1k, the AUX-present divider). The offset
  between the grounds is the voltage drop on the main board between the card and NT1 (tens of mV), well inside the
  logic margins.
- **On the bench:** a card powered without the main board needs AUX GND and GND joined at the supply, or the core
  board's ground floats.

## Slot connector (J1)

| Pins | Signal | Used |
| --- | --- | --- |
| 1 / 2 | +5 V | ESP32-S3, CAN, 3.3 V regulator |
| 3 / 4, 7, 19 / 20 | GND | logic ground |
| 5 / 6 | CANH / CANL | MCP2562 |
| 11 | slot ID | 10k to 3.3 V, GPIO4 |
| 25 / 26 / 27 | AUX 12 V | the LTE buck |
| 28 / 29 / 30 | AUX GND | the LTE buck and core board return |
| 8, 9, 10, 12–18, 21–24 | switched / constant 12 V, 3.3 V, WAKE, LAMP, REV, links, audio bus | not connected |

| Other connectors | | |
| --- | --- | --- |
| J2 | LTE core board row A, 1 × 12 female, 8.5 mm | provisional pin order (see above) |
| J3 | LTE core board row B, 1 × 11 female, 8.5 mm | provisional pin order |
| J4 | USB-C, mid-mount, left edge (Amphenol 12401948E412A) | native USB: flash and serial console. The card is powered from the slot |
| J5 | debug serial, 1 × 3 right angle, left edge | 1 GND, 2 TX0 (to the adapter's RX), 3 RX0. 3.3 V logic, not a supply |
| SW1 / SW2 | RESET / BOOT, beside the USB-C | Flash: hold BOOT, tap RESET (only needed if the USB-JTAG is not answering) |

## Board

- **Outline:** 80 × 60 mm, no mounting holes. A notch in the left edge for the mid-mount USB-C (it is in the
  footprint's own Edge.Cuts).
- **Slot header (J1):** placed exactly as on the audio card (`PinHeader_2x15_P2.54mm_Horizontal`, pin 1 17.78 mm right
  of the centre, the pins counting to the left, its body on the bottom edge); `gen_pcb.py` checks the orientation from
  the pads.
- **Top left:** the module, antenna end on the top edge at the corner, with the stock footprint's keep-out (no copper,
  vias or parts on any layer, 15 mm to its right; nothing on the back under it). Keep metal away from that corner of
  the case. The core board starts right of the keep-out.
- **Top right:** the core board on its sockets. Nothing is placed under it.
- **Left edge:** LEDs (under the module), USB-C, RESET / BOOT, the debug header.
- **Bottom:** the buck by the AUX pins (25–30), the 470 µF under the core board's VIN pins, the CAN transceiver and
  the 3.3 V regulator by pins 1–6.
- **Back:** only the module's 22 µF / 100 nF (C3 / C4), behind its 3V3 pin: there is no room between the module's pads
  and the edge.
- **Layers:** F signal, In1 solid GND, In2 signal / pours, B signal. 0.2 mm tracks, 0.13 mm clearance, 0.6 / 0.3 mm
  vias (as the audio card). Net classes as on the main board: Power 0.3 mm (GND, +5V, +3V3); Aux 1.0 mm / 0.2 mm
  (AUX12, LTE_5V); AuxGnd 0.8 mm / 0.2 mm (AUX_GND); Switch 1.0 mm / 0.2 mm (BUCK_SW).
- **Antenna (LTE):** nothing RF on the card: an IPEX-to-SMA pigtail from the core board to the case panel.

## Status

**Schematic: done** (63 parts, ERC: no errors; the warnings are off-grid pins and the generated `LTECard` symbol
library not being in the library table). **PCB: placed and routed** by `gen_pcb.py` (Freerouting 1.9.0 plus the
script's finisher and stitching, no hand edits). DRC with the schematic parity check (2026-10-01): **0 errors,
0 unconnected**, warnings only: 21 silkscreen-past-the-edge (J1, J4 and J5 stick out of the outline on purpose), 2 LED
legend / LED outline overlaps, and the module footprint differing from the library (its 0.2 mm ground-pad vias are
opened to 0.3 mm). Three short pieces of the switch node were necked to 0.6 mm by the router at the pads.
**Not ordered:** the core board items above come first.

Also check before ordering:
- the buck: L1's ratings (Isat ≥ 5 A, Irms ≥ 3 A); the compensation is the main board's (TI typical application for
  7.2 µH and 3 × 47 µF) with a 6.8 µH inductor and the 470 µF electrolytic added at the load: check the loop (TI
  WEBENCH, or a load-step test) once the parts are known.
- pin 1 of J1 against the main board's slot in KiCad's 3D viewer, and the USB-C notch against the case.
- the module's ground-pad vias were opened from the footprint's 0.2 mm to 0.3 mm drills (the board's minimum).

## Regenerating

KiCad 10 with its stock libraries, numpy in KiCad's python, and Freerouting **1.9.0** (Java 17 or newer; it opens its
window while it routes). Temp files go to `$TMPDIR` with `ltecard` in their names.

```bash
KIPY=/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3
python3 generate/gen_sch.py        # also writes generate/refs.json (part groups for placement)
JAVA=/opt/homebrew/opt/openjdk@17/bin/java $KIPY generate/gen_pcb.py --route freerouting-1.9.0.jar   # place, route, fill
python3 generate/drc.py
```

`FR_PASSES` sets Freerouting's pass limit (default 120). `--stitch` / `--fill` re-run only the ground stitching / zone
fill on the routed board. Hand fixes after the routing (if any) are listed under [Status](#status) and are lost on a
re-run.
