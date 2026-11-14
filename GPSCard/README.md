# GPS card

A **slot card** for the DigiDash rev 2 board: **80 × 60 mm**, standing on edge in a 2×15 slot, its right-angle header
on the bottom edge and the GPS connector on the top edge (`docs/hardware/accessory_ports.md`, rev 2 section). It reads
a **remote GPS puck** (receiver and antenna in one housing, on the dash under the windshield) over a 4-pin cable and
sends position, motion, quality, time and status on CAN (`docs/hardware/gps_card.md` is the spec). Any slot. On it:
- an **STM32G0B1KBU6** (UFQFPN-32, Cortex-M0+, built-in FDCAN) with an 8 MHz oscillator into PC14 (HSE bypass)
- an **MCP2562** CAN transceiver (VDD 5 V, VIO 3.3 V, STBY low, no termination)
- an **AP2112K-3.3** regulator from the slot's 5 V
- the puck connector **J2** (JST PH 4-pin side entry, S4B-PH-K, same pinout as the main board's J6), with a 3.3 V / 5 V
  supply jumper, a 0.2 A PTC, 100 Ω series resistors and a USBLC6-2SC6 ESD array
- **J3** SWD and **J4** debug serial, both right angle on the left edge, and one status LED

No radio and no RF parts. CAN node from its slot (`0x14 + (slot − 1)`), 5 V from the slot only.

| Files | |
| --- | --- |
| `GPSCard.kicad_sch` / `.kicad_pcb` / `.kicad_pro` | The card: 80 × 60 mm, 2 layers, all parts on the parts side (F). KiCad 10 |
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
| 2 | PC14 | OSC_IN (HSE bypass) | — | Y1 pin 3, 8 MHz oscillator |
| 3 | PC15 | unused | — | — (the 32-pin package has no OSC_OUT) |
| 4 | VDD / VDDA | 3.3 V | — | 100 nF + 4.7 µF |
| 5, 33 (pad) | VSS / VSSA | GND | — | |
| 6 | PF2 | NRST | — | J3 pin 4, 100 nF |
| 7 | PA0 | ADC1_IN0 | analog | slot ID |
| 9 | PA2 | USART2_TX | AF1 | puck (J2 pin 2) |
| 10 | PA3 | USART2_RX | AF1 | puck (J2 pin 3) |
| 12 | PA5 | GPIO | — | LED D1 (high = on, 1k) |
| 19 | PA9 | USART1_TX | AF1 | J4 pin 2 |
| 21 | PA10 | USART1_RX | AF1 | J4 pin 3 |
| 22 | PA11 | FDCAN1_RX | AF3 | MCP2562 RXD (through 1k) |
| 23 | PA12 | FDCAN1_TX | AF3 | MCP2562 TXD |
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
- **Layers:** 2 (from 2026-10-05; rev A was drawn on 4), for the price: a 2-layer 80 × 60 mm prototype costs a
  fraction of a 4-layer one. F carries the routing and a GND pour; B is the ground reference: a GND pour with as
  little routing as the router needs (about 94 mm of track, against 587 mm on F), and it fills as one piece over
  about 95 % of the board. GND is not routed: `rules()` gives it its own net class (`Ground`) and Freerouting runs
  with `-inc Ground`; every GND pin gets its own via to B before routing, and the pours join the rest. 0.2 mm tracks,
  0.3 mm on the supplies (+5V, +3V3, puck power), 0.13 mm clearance, 0.6 / 0.3 mm vias: standard 2-layer rules at
  JLCPCB / PCBWay.
- Ground stitching F to B on a 4 mm grid (the audio card uses 2 mm; this board is nearly empty).

## Status

**Schematic: done** (28 parts). **PCB: placed and routed**, DRC clean: 0 errors, 0 unconnected, schematic parity
clean; the only warnings are silkscreen past the board edge (the right-angle headers' and J2's silk outlines, which
stand past the edges on purpose). ERC: no errors (warnings are off-grid pins of the stock symbols and the `GPSCard`
symbol nickname not being in a library table: the derived stock symbols are flattened into the schematic under it).

### Check before ordering

- **MCU pin functions: verified** against ST's CubeMX pin database (`STM32_open_pin_data`: `STM32G0B1K(B-C-E)Ux.xml`
  and `GPIO-STM32G0Bx_gpio_v1_0_Modes.xml`) and the schematic netlist: FDCAN1 on PA11 / PA12 (pins 22 / 23, **AF3**;
  an earlier note said AF9, which was wrong), USART2 on PA2 / PA3 (AF1), USART1 on PA9 / PA10 (pins 19 / 21, AF1),
  HSE in on PC14 (pin 2; **bypass only**: the first version wrongly put a crystal on PC14 / PC15, see below), NRST on PF2 (pin 6), PA0 = ADC1_IN0, PA13 / PA14 SWD (AF0), VDD / VSS pins 4 / 5.
  **Order the plain KBU6, not KBU6N:** the N variant has a different pinout (pin 20 is VDDIO2, pins 26–29 are PD0–PD3).
  Confirmed again against DS13560 Rev 6 (Figure 4 pinout, Tables 13 / 14 port A AFs).
- **U1 footprint: checked** against DS13560 section 6.3 (A0B8). Package exposed pad 3.50 mm nominal (3.40–3.60); the
  KiCad land is 3.45 × 3.45 with 9 paste windows (about 65 % coverage), pads 0.25 × 0.875 at 0.5 mm pitch. KiCad's pad
  row sits 0.125 mm further out than ST's example land (5.75 / 4.00 mm outer / inner span vs ST's 5.50 / 3.75), which
  leaves a wider gap to the exposed pad. Either is fine for assembly; no change.
- **No via in any pad: checked.** The pre-routing GND fan-out had put 4 GND vias in pads (2 in U1's exposed pad,
  2 on Y1 pad 2); they and their stubs were removed. U1's EP still reaches GND through pin 5's track, Y1's GND pad
  through the F.Cu pour; every GND pad still reaches the ground plane. `gen_pcb.py`'s `stitch()` no longer treats big GND
  pads as free space and no longer falls back to a via in the pad. Closest via to a pad of another net: 0.139 mm.
- **Other parts' pinouts: checked** against the netlist: MCP2562 (1 TXD, 2 VSS, 3 VDD 5 V, 4 RXD, 5 VIO 3.3 V,
  6 CANL, 7 CANH, 8 STBY low), AP2112K (1 VIN, 2 GND, 3 EN, 5 VOUT), USBLC6-2SC6 (1 / 6 TX, 3 / 4 RX, 5 VBUS = puck
  power, 2 GND), oscillator (1 tri-state to 3V3, 2 GND, 3 OUT, 4 VDD), LED cathode to GND, JP1 1 = 3V3, 3 = 5 V.
- **J1 vs the main board: checked.** Pins 1 / 2 +5 V, 3 / 4 / 7 / 19 / 20 GND, 5 CANH, 6 CANL, 11 slot ID match
  DigiDash2's J31–J36 slot nets. Placement matches `accessory_ports.md` (pin 1 at x + 17.78 mm, pad 2 below, pad 3
  to the left) and is identical to the audio and sensor cards' J1.
- **J2: checked.** Pinout matches DigiDash2's J6 (1 3V3, 2 TX out, 3 RX in, 4 GND); rotated 180° so the body runs to
  the top edge and the plug opening faces off the board; pad 1 on the right.
- **Clock: YXC OT32258MJBA4SL oscillator** (LCSC C717687), not a crystal; see "Clock" under Status.
- **BOM: complete.** `fab/GPSCard_BOM_PCBWay.csv` (PCBWay format, every line with maker and part number, matches the
  26 placed parts; `tools/fab_export.py` warns if it drifts) and `GPSCard_BOM.csv` (same parts, plus JP1).
- **Still by eye:** J2 against the case panel cut-out (J2 is centred at x 40); J3 / J4 (their right-angle pins stand
  about 6.5 mm past the left edge) against the neighbouring card and the case. KiCad's 3D viewer.
- **JP1:** delivered bridged 1-2 (3.3 V puck). Most NEO-6M / M8 boards are fine on 3.3 V; drone-style pucks often want
  5 V. USBLC6-2SC6's VBUS (puck supply) is rated to 5.5 V.
- **Puck RX with no puck:** PA3 floats when nothing is plugged in; enable its internal pull-up in firmware.
- The card outline (80 × 60 mm) against the case.

**Clock: an oscillator, not a crystal (fixed 2026-11-13).** This card was first drawn with an 8 MHz crystal and two load
caps on PC14 / PC15. **That crystal can never start**: the 32-pin STM32G0B1 has no PF0 / PF1, so no HSE crystal driver.
ST's pin data gives PC14 `RCC_OSC_IN` and PC15 only `RCC_OSC_EN` / `RCC_OSC32_OUT`, no `RCC_OSC_OUT`; an external clock
into PC14 in HSE bypass is the only external HSE. Found on the first SensorCard2 boards (same chip, same mistake).
Now Y1 is a **YXC OT32258MJBA4SL** (LCSC C717687): 8 MHz CMOS oscillator, 3.2 × 2.5 mm, 1.8-3.3 V, ±20 ppm, -40 to
85 °C. Pinout from YXC's datasheet (YSO110TR series): 1 tri-state (high or open = on, tied to 3V3), 2 GND, 3 OUT (to
PC14), 4 VDD, with a 100 nF (C8) beside it. Footprint `Oscillator_SMD_Abracon_ASE-4Pin_3.2x2.5mm`: the same pin order,
pads within 0.15 mm of YXC's land pattern. PC15 is unused. The board was edited in place: the old PC15 route now
feeds the oscillator's 3V3, C7 is gone. This card's firmware (not written yet) must select `RCC_HSE_BYPASS`, as `firmware/sensor_card_stm32/src/hw_sys.cpp` does.

## Things to know

- **Firmware is not written yet.** See `docs/hardware/gps_card.md` (auto-baud on USART2, UBX-NAV-PVT at 5 Hz or NMEA,
  CAN `0x222`–`0x226`, heartbeat `0x700 + node`, NET ANNOUNCE as product `0x0005`). Build with STM32duino (generic G0B1KBUx
  variant) or STM32Cube. FDCAN clock from the 8 MHz HSE (oscillator, bypass mode) through the PLL.
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
