# AM/FM radio card

A **slot card** for the DigiDash rev 2 board: **80 × 60 mm**, standing on edge in a 2×15 slot, its right-angle header
on the bottom edge and the antenna jack on the top edge (`docs/hardware/accessory_ports.md`, rev 2 section). Any
slot. Spec: [`docs/hardware/radio_card.md`](../docs/hardware/radio_card.md). On it:
- a **Si4735-D60-GU** AM/FM/SW/LW tuner with RDS (SSOP-24), controlled over I2C, its digital audio a slave on the
  slot I2S rail's clocks, **its reference clock taken from the rail's BCK** (no oscillator)
- an **STM32G0B1KBU6** (FDCAN) with an 8 MHz crystal, an **MCP2562** CAN transceiver and an **AP2112K-3.3** from the slot's 5 V
- a **74LVC1G125** that puts the tuner's audio on the rail's RET line, only after the audio card grants it
- **J2** SMA edge mount (Amphenol 132289) with a 0.13 pF ESD clamp and AN383's 56 nH FM match, **J5** for an AM ferrite
  loopstick, **J3** SWD and **J4** debug serial on the left edge, two LEDs

CAN node from its slot (`0x14 + (slot − 1)`), product `0x0009`, node type 9. 5 V from the slot only.

| Files | |
| --- | --- |
| `RadioCard.kicad_sch` / `.kicad_pcb` / `.kicad_pro` | The card: 80 × 60 mm, 2 layers, all parts on the parts side (F). KiCad 10 |
| `RadioCard_BOM.csv` | Parts list, every part with maker and part number (plus the PCB jumpers and the not-fitted R13) |
| `fab/` | Gerbers, drill, placement and centroid (`python3 tools/fab_export.py RadioCard`), and the assembler's BOM `RadioCard_BOM_PCBWay.csv` (the 48 placed parts) |
| `generate/` | Scripts that produced the files (`gen_sch.py`, `gen_pcb.py`, `drc.py`, `sexp.py`; `refs.json` / `netlist.json` are written by `gen_sch.py`). Copied from `GPSCard/generate/`. Every symbol and footprint is stock KiCad 10 |

## Signal paths

```
J2 SMA centre ── D2 ESD (0.13 pF) ── L1 56 nH to GND ── C16 1 nF ── U4 FMI (pin 8)
J5 pin 1 (loopstick, 180-450 uH to GND) ── C17 470 nF ── U4 AMI (pin 12)        JP2 (open): SMA to the AM node
Slot BCK (15) ── R7 2k ── U4 DCLK (pin 3)   also the tuner's reference: REFCLK_PRESCALE 0x1060, REFCLK_FREQ 32000
Slot WS (16)  ── R6 2k ── U4 DFS (pin 2)    C14 2.2 pF on the WS line; 100k idle pull-downs on BCK and WS
U4 DOUT (1) ── R5 604 ── U5 74LVC1G125 (OE = PB2, pulled off) ── R16 33 ── slot RET (18)
Slot MCLK (24) ── R13 0 R, NOT FITTED ── U4 RCLK (19), 100k to GND (fallback reference, prescale 375)
U1 PB6 / PB7 (I2C1, AF6) ── U4 SCLK / SDIO (17 / 18), 4.7k pull-ups; SEN by JP1 (0x11 as delivered)
U1 PB0 ── U4 RST (15), 100k to GND      U1 PB1 ◄── U4 GPO2 / INT (4)
Slot CANH / CANL (5 / 6) ── U2 MCP2562 ── PA12 FDCAN1_TX / PA11 FDCAN1_RX (AF3, RX through 1k)
Slot ID (11): 10k to 3.3 V here ──► PA0 ADC1_IN0
Slot +5 V ── U3 AP2112K-3.3 ── +3V3 (MCU, VIO, tuner VD) ── FB1 ── +3V3A (tuner VA; C12 22 nF DBYP to VA)
```

### MCU pins (STM32G0B1KBU6, UFQFPN-32, "GP" pinout, DS13560 rev 4)

| Pin | Port | Function | AF | To |
| --- | --- | --- | --- | --- |
| 2 / 3 | PC14 / PC15 | OSC_IN / OSC_OUT (HSE) | — | Y1 8 MHz (YXC X32258MOB4SI, CL 12 pF), 12 pF caps |
| 4 | VDD / VDDA | 3.3 V | — | 100 nF + 4.7 µF |
| 5, 33 (pad) | VSS | GND | — | |
| 6 | PF2 | NRST | — | J3 pin 4, 100 nF |
| 7 | PA0 | ADC1_IN0 | analog | slot ID |
| 12 | PA5 | GPIO | — | D1 TUNED (high = on) |
| 13 | PA6 | GPIO | — | D3 STEREO / RDS (high = on) |
| 15 | PB0 | GPIO out | — | tuner RST (low = reset; 100k pull-down) |
| 16 | PB1 | GPIO in | — | tuner GPO2 / INT |
| 17 | PB2 | GPIO out | — | RET buffer OE, **active low**, 10k pull-up (off) |
| 19 / 21 | PA9 / PA10 | USART1 TX / RX | AF1 | J4 |
| 22 / 23 | PA11 / PA12 | FDCAN1 RX / TX | **AF3** | MCP2562 (SYSCFG remap off) |
| 24 / 25 | PA13 / PA14 | SWDIO / SWCLK (BOOT0) | AF0 | J3 |
| 30 / 31 | PB6 / PB7 | I2C1 SCL / SDA | AF6 | tuner |

| Connector | Pins |
| --- | --- |
| J1, slot (2×15, right angle, bottom edge) | Used: 1 / 2 +5 V, 3 / 4 / 7 / 19 / 20 GND, 5 CANH, 6 CANL, 11 slot ID, 15 BCK, 16 WS, 18 RET, 24 MCLK (through unfitted R13). Everything else open |
| J2, antenna (SMA, top edge) | Centre: FM (and AM with JP2); shell: GND. The car whip's Motorola plug goes in through a short pigtail, DIN 41585 female to SMA male (not RP-SMA), e.g. Eightwood TB0188 (10 cm RG174); no straight adapter is sold |
| J5, AM loopstick (1 × 2, right angle, top edge) | 1 AM node (left, square pad), 2 GND |
| J3, SWD (1 × 5, right angle, left edge) | 1 3V3 (sense), 2 SWDIO, 3 SWCLK, 4 NRST, 5 GND (pin 1 lowest) |
| J4, debug serial (1 × 3, right angle, left edge) | 1 GND, 2 TX (out), 3 RX (in) |

## Board

- **80 × 60 mm, 2 layers** (from 2026-10-05; rev A was drawn on 4), for the price. F carries most of the routing and
  a GND pour; B is the ground reference: a GND pour that fills as one piece over about 91 % of the board, with the
  rest of the routing (about 340 mm, against 660 mm on F). GND is not routed: it has its own net class (`Ground`),
  Freerouting runs with `-inc Ground`, every GND pin gets its own via to B first, and the pours join the rest.
  0.2 mm tracks, 0.3 mm supplies, 0.13 mm clearance, 0.6 / 0.3 mm vias: standard 2-layer rules.
- **The one 2-layer compromise:** a bundle of 4–5 digital lines (the tuner's I2S / I2C / clock to the MCU) crosses the
  middle of B diagonally. The B pour joins around it and stitching vias run beside it, but it is a slot in the ground
  reference there. Fine for these signals; if FM reception on the bench picks up digital noise, look here first.
- **RF end at the top:** J2 at x 45, J5 at x 33, the tuner (U4) below them turned so FMI / AMI face the SMA. A rule
  area on B (`RF_KEEP`: x 28–62, y 0–12.5) keeps tracks out from under the front end; only the GND pour and stitching
  vias go there, so the ground under the antenna path is solid.
- **Bottom:** the MCU, the CAN transceiver over the header's CAN pins, the regulator over the 5 V pins, the RET buffer
  over the RET pin.
- No parts on the back; no mounting holes (the slot and the case hold it).

## Status

**Schematic and board done** (2026-10-02). ERC: no errors (the endpoint-off-grid and project-library warnings
every generated card has). PCB placed and routed (Freerouting 1.9.0, complete). Re-routed on 2 layers 2026-10-05: one Freerouting round, complete; 35 GND fan-out vias and 201 stitching vias, none in a pad.
DRC with the schematic parity check: **no errors, nothing unconnected**; J2's five edge-clearance hits are expected
(edge-launch SMA, reported apart by `drc.py`); 32 silkscreen warnings. Fab outputs in `fab/` (48 placed parts; R13 is
not fitted).

Check before ordering:
- The SMA's land pattern and which line is the board edge (the footprint puts the edge 2.54 mm in front of pad 1;
  `gen_pcb.py` places it so), against the jack in hand and the case panel.
- D2's 0402 land pattern against TI's X1SON (DPY) drawing.
- Pin 1 of J1 against the main board's slot in KiCad's 3D viewer.

Open (spec, [Open](../docs/hardware/radio_card.md#open)): an AM network for the car whip (JP2), and tuning L1 on the
car's whip and cable.

## Things to know

- **The tuner needs the rail's clocks before it can do anything**: they are its reference clock as well as its audio
  clocks. The firmware powers it up after the audio card is running, and sets the output sample rate to 0 before
  the clocks stop (AN332: otherwise the chip needs a reset).
- **RET is driven only after a NET RAIL grant.** U5 is off at reset and while the MCU is blank (R15 pull-up on OE).
- The STM32 comes from assembly blank: program it over J3 (SWD), powered from the slot. J4 is the debug serial and the
  ROM bootloader fallback.
- `tools/can_relay.py` simulates this card on the bench (`tools/radio_sim.py`).

## Regenerating

KiCad 10 with its stock libraries, numpy in KiCad's python, and Freerouting **1.9.0** (Java 17 or newer).

```bash
KIPY=/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3
python3 generate/gen_sch.py          # also writes generate/refs.json and netlist.json
JAVA=/opt/homebrew/opt/openjdk@17/bin/java FR_PASSES=60 $KIPY generate/gen_pcb.py --route freerouting-1.9.0.jar
python3 generate/drc.py
python3 ../tools/fab_export.py RadioCard
```
