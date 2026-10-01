# GPS card

A **slot card** for the DigiDash rev 2 board: **80 × 60 mm**, standing on edge in a 2×15 slot, its right-angle header
on the bottom edge and the GPS connector on the top edge (`docs/hardware/accessory_ports.md`, rev 2 section). It reads
a **remote GPS puck** (receiver and antenna in one housing, on the dash under the windshield) over a 4-pin cable and
sends position, motion, quality, time and status on CAN (`docs/hardware/gps_card.md` is the spec). Any slot. On it:
- an **STM32G0B1KBU6** (UFQFPN-32, Cortex-M0+, built-in FDCAN) with an 8 MHz crystal
- an **MCP2562** CAN transceiver (VDD 5 V, VIO 3.3 V, STBY low, no termination)
- an **AP2112K-3.3** regulator from the slot's 5 V
- the puck connector **J2** (JST PH 4-pin side entry, S4B-PH-K, same pinout as the main board's J6), with a 3.3 V / 5 V
  supply jumper, a 0.2 A PTC, 100 Ω series resistors and a USBLC6-2SC6 ESD array
- **J3** SWD and **J4** debug serial, both right angle on the left edge, and one status LED

No radio and no RF parts. CAN node from its slot (`0x14 + (slot − 1)`), 5 V from the slot only.

| Files | |
| --- | --- |
| `GPSCard.kicad_sch` / `.kicad_pcb` / `.kicad_pro` | The card: 80 × 60 mm, 4 layers, all parts on the parts side (F). KiCad 10 |
| `GPSCard_BOM.csv` | Parts list |
| `generate/` | Scripts that produced the files (`gen_sch.py`, `gen_pcb.py`, `drc.py`, `sexp.py`; `refs.json` is written by `gen_sch.py`). Copied from `AudioCarrier/generate/` and adapted. Every symbol and footprint is stock KiCad 10, so there is no `gen_footprints.py` and no project library |

## Signal paths

```
J2 pin 2 (TX, to the puck's RX) ── R4 100 R ── PA2  USART2_TX (U1 pin 9)
J2 pin 3 (RX, from the puck's TX) ── R5 100 R ── PA3  USART2_RX (U1 pin 10)    U4 USBLC6-2SC6 on J2 pins 1, 2, 3
JP1 (1-2 = +3V3 as delivered, 2-3 = +5V) ── F1 0.2 A PTC ── J2 pin 1 (puck power, 1 uF)
Slot CANH / CANL (5 / 6) ── U2 MCP2562 ── TXD ◄ PA12 FDCAN1_TX (pin 23),  RXD ── 1k ──► PA11 FDCAN1_RX (pin 22)
Slot ID (11): 10k to 3.3 V here, the slot's resistor to GND on the main board ──► PA0 ADC1_IN0 (pin 7)
Slot +5 V (1 / 2) ── U3 AP2112K-3.3 ── +3V3 (MCU, MCP2562 VIO, a 3.3 V puck)
```

### MCU pins (STM32G0B1KBU6, UFQFPN-32)

| Pin | Port | Function | AF | To |
| --- | --- | --- | --- | --- |
| 2 | PC14 | OSC_IN (HSE) | — | Y1 8 MHz, 12 pF |
| 3 | PC15 | OSC_OUT (HSE) | — | Y1, 12 pF |
| 4 | VDD / VDDA | 3.3 V | — | 100 nF + 4.7 µF |
| 5, 33 (pad) | VSS / VSSA | GND | — | |
| 6 | PF2 | NRST | — | J3 pin 4, 100 nF |
| 7 | PA0 | ADC1_IN0 | analog | slot ID |
| 9 | PA2 | USART2_TX | AF1 | puck (J2 pin 2) |
| 10 | PA3 | USART2_RX | AF1 | puck (J2 pin 3) |
| 12 | PA5 | GPIO | — | LED D1 (high = on, 1k) |
| 19 | PA9 | USART1_TX | AF1 | J4 pin 2 |
| 21 | PA10 | USART1_RX | AF1 | J4 pin 3 |
| 22 | PA11 | FDCAN1_RX | AF9 | MCP2562 RXD (through 1k) |
| 23 | PA12 | FDCAN1_TX | AF9 | MCP2562 TXD |
| 24 | PA13 | SWDIO | AF0 | J3 pin 2 |
| 25 | PA14 | SWCLK (and BOOT0) | AF0 | J3 pin 3 |

All other pins are left open (no-connect in the schematic); set them to analog in firmware. Pins 22 / 23 are PA11 /
PA12 with the SYSCFG remap **off** (the remap would put PA9 / PA10 there). USART1 on PA9 / PA10 and USART2 on PA2 / PA3
are also ROM bootloader ports.

| Connector | Pins |
| --- | --- |
| J1, slot (2×15, right angle, bottom edge) | Used: 1 / 2 +5 V, 3 / 4 / 7 / 19 / 20 GND, 5 CANH, 6 CANL, 11 slot ID. Everything else open |
| J2, GPS puck (JST PH S4B-PH-K, top edge) | 1 puck power (3.3 V, or 5 V by JP1; 0.2 A PTC), 2 TX (out, to the puck's RX), 3 RX (in, from the puck's TX), 4 GND. Pad 1 is on the right, seen from the parts side |
| J3, SWD (1×5, 2.54 mm, right angle, left edge) | 1 3V3 (target sense, not a supply), 2 SWDIO, 3 SWCLK, 4 NRST, 5 GND. Pin 1 is the lowest pin |
| J4, debug serial (1×3, 2.54 mm, right angle, left edge, below J3) | 1 GND, 2 TX (out of the card), 3 RX (into the card); USART1, 3.3 V. Pin 1 is the lowest pin |
| JP1, puck supply | Pads 1-2 joined by a thin copper trace as delivered = 3.3 V. For a 5 V puck: cut the trace between 1 and 2, bridge 2-3 with solder. Silk "3V3" / "5V" |

## Board

- **Outline:** 80 × 60 mm, no mounting holes: the slot and the case hold it.
- **Slot header (J1):** right-angle 2×15 (`PinHeader_2x15_P2.54mm_Horizontal`) on the parts side, centred on the bottom
  edge, placed exactly as on the audio card: pin 1 is 17.78 mm right of the centre (seen from the parts side, top edge
  up), the pins count to the left, the plastic body's outer face is on the bottom edge and the pins stand 6 mm past it.
  `gen_pcb.py` checks the orientation from the pads (same check as the audio card's), so each pin meets the
  same-numbered pad of the main board's slot socket.
- **Top edge** (the case panel): J2, centred at x 40 (constant `J2_X` at the top of `gen_pcb.py`), its plug opening on
  the edge. U4 (ESD), R4 / R5, F1, C11 and JP1 sit right behind it.
- **Left edge:** J3 (pins at y 15.8–26) and J4 (y 30.9–36), right angle, the plastic body on the edge and the pins
  6 mm past it, so they can be reached with the card in its slot. Pin legends on the silk beside them.
- **Bottom:** U2 (CAN) above the slot's CAN pins, U3 (3.3 V) above the 5 V pins, the MCU in the middle-left between the
  headers and the transceiver. R2 and the LED D1 sit straight below U1 pin 12 (PA5), on the open side of the QFN
  (fixed in `FIXED`: placed by the passive placer, R2 ended up beside the pin and Freerouting could not reach it).
- **Parts side (F):** everything. Tallest part: J2 (about 6 mm) and the right-angle headers (about 2.5 mm), well under
  the 20 mm limit. **Back (B):** no parts, only copper and the card name.
- **Layers:** 4, the audio card's stack and rules: F signal, In1 solid GND, In2 signal / GND fill, B signal / GND fill.
  0.2 mm tracks, 0.3 mm on the supplies (+5V, +3V3, puck power), 0.13 mm clearance, 0.6 / 0.3 mm vias: standard
  4-layer rules at JLCPCB / PCBWay. The spec says 2 layers are enough; 4 was used because the audio card's generator
  (router set-up, In1 plane, stitching) works unchanged, and so the card can share a panel with the others. A 2-layer
  version would need `gen_pcb.py`'s plane and stitching steps reworked.
- Ground stitching on a 4 mm grid (the audio card uses 2 mm; this board is nearly empty).

## Status

**Schematic: done** (28 parts). **PCB: placed and routed**, DRC clean: 0 errors, 0 unconnected, schematic parity
clean; the only warnings are silkscreen past the board edge (the right-angle headers' and J2's silk outlines, which
stand past the edges on purpose). ERC: no errors (warnings are off-grid pins of the stock symbols and the `GPSCard`
symbol nickname not being in a library table: the derived stock symbols are flattened into the schematic under it).

### Check before ordering

- **MCU pin functions** against ST's datasheet (DS13560, STM32G0B1xB/xC/xE, the UFQFPN32 pinout and the
  alternate-function tables): FDCAN1 on PA11 / PA12 (AF9), USART2 on PA2 / PA3 (AF1), USART1 on PA9 / PA10 (AF1),
  HSE on PC14 / PC15 (the 32-pin package has no PF0 / PF1, so OSC_IN / OSC_OUT share PC14 / PC15), PA0 = ADC1_IN0.
  KiCad's symbol gives only the port names, not the AFs; these were taken from the datasheet as I know it and should
  be confirmed in STM32CubeMX (open a G0B1KBUx project and assign the same pins) before ordering.
- **Crystal load caps:** C7 / C8 are 12 pF for a CL 10 pF crystal. Change them with the crystal you buy.
- **J2 orientation:** the plug opening must face off the top edge. Check in KiCad's 3D viewer and against the case
  panel cut-out (J2 is centred at x 40). Also check J1 pin 1 against the main board's slot (3D viewer, as for the audio
  card), and that J3 / J4 clear the neighbouring card and the case.
- **JP1:** delivered bridged 1-2 (3.3 V puck). Most NEO-6M / M8 boards are fine on 3.3 V; drone-style pucks often want
  5 V. USBLC6-2SC6's VBUS (puck supply) is rated to 5.5 V.
- **Puck RX with no puck:** PA3 floats when nothing is plugged in; enable its internal pull-up in firmware.
- The card outline (80 × 60 mm) against the case.

## Things to know

- **Firmware is not written yet.** See `docs/hardware/gps_card.md` (auto-baud on USART2, UBX-NAV-PVT at 5 Hz or NMEA,
  CAN `0x222`–`0x226`, heartbeat `0x700 + node`, NET ANNOUNCE as product `0x0005`). Build with STM32duino (generic G0B1KBUx
  variant) or STM32Cube. FDCAN clock from the 8 MHz HSE through the PLL.
- **Flashing:** the STM32 comes from assembly blank. Use J3 with an ST-Link (V2 clones work); power the card from the
  slot, J3 pin 1 is only the voltage sense. With blank flash the G0 starts its ROM bootloader by itself, so J4 (USART1)
  also works with STM32CubeProgrammer over a USB-serial adapter. No BOOT0 strap: on the G0 it shares SWCLK and the
  option bytes choose.
- **Slot ID:** read PA0 at boot (10k up here, 1k … 47k down on the main board: about 0.30 … 2.72 V) and put the slot in
  the NET ANNOUNCE flags.
- **LED (PA5, high = on):** off = nothing from the puck, blinking = searching, steady = 3D fix.

## Regenerating

KiCad 10 with its stock libraries, numpy in KiCad's python (`$KIPY -m pip install --user numpy`), and Freerouting
**1.9.0** (Java 17 or newer; it opens its window while it routes). 1.9 hangs if the DSN already has wiring, so each
round routes once from the placed, unrouted board and then finishes with the script's own steps (joining any leftover
piece of a net, ground vias and stitching). One Freerouting round (pass limit `FR_PASSES`, default 120) routes this
placement completely. If a placement change leaves a link unrouted, fix the placement (the parts fixed by hand are in
`FIXED` at the top of `gen_pcb.py`); `FR_TRIES=n` (default 1) re-runs Freerouting from the placed board up to n times,
cycling through a comma-separated `FR_PASSES` list, but that is a stopgap.

```bash
KIPY=/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3
python3 generate/gen_sch.py                       # also writes generate/refs.json (part groups for placement)
kicad-cli sch erc GPSCard.kicad_sch               # expect warnings only (off-grid, library nickname)
JAVA=/opt/homebrew/opt/openjdk@17/bin/java $KIPY generate/gen_pcb.py --route freerouting-1.9.0.jar   # place, route, fill
python3 generate/drc.py                           # DRC with the schematic parity check
```

Temporary files (netlist, DSN / SES, DRC reports) go to `$TMPDIR` with a `gpscard` prefix. `--stitch` / `--fill`
re-run only the ground stitching / zone fill on the routed board. After any schematic change run the generators again
(or, in KiCad, **Update PCB from Schematic** and re-route by hand).
