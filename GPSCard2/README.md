# GPS card, rev B (through-hole)

The hand-assembled version of the [GPS card](../GPSCard/README.md). Same job, same **80 × 60 mm slot card**, same slot
header (J1, bottom edge), puck connector (J2, top edge) and CAN IDs (`docs/hardware/gps_card.md`). The difference is that
**every part is through-hole**, so you can solder the whole board yourself:
- a **PIC18F26Q83-I/SP** (SPDIP-28, in a socket): PIC18 at 64 MHz with its own **CAN FD controller** (run as classic CAN
  at 500 kbit/s) and 5 UARTs, with a 16 MHz HC-49 crystal (4x PLL)
- an **MCP2562-E/P** CAN transceiver (DIP-8; VDD 5 V, VIO 3.3 V, STBY low, no termination)
- an **MCP1700-3302E/TO** 3.3 V regulator (TO-92) from the slot's 5 V
- the puck connector **J2** (JST PH S4B-PH-K, as rev A), a **3-pin header + shunt** for the 3.3 V / 5 V puck supply,
  a radial 0.25 A PTC and 100 Ω series resistors
- **J3, a 6-pin Microchip ICSP header** (PICkit 4 / 5, MPLAB Snap) and **J4** debug serial, both right angle on the
  left edge, and a 3 mm status LED
- axial 1/4 W resistors (10.16 mm pitch) and radial capacitors (2.54 mm lead spacing)

No ESD array: the USBLC6-2SC6 on rev A has no through-hole version. The 100 Ω series resistors and the PIC's own clamp
diodes protect the puck lines; the cable is short and stays inside the cab.

| Files | |
| --- | --- |
| `GPSCard2.kicad_sch` / `.kicad_pcb` / `.kicad_pro` | The card: 80 × 60 mm, 2 layers, every part on the parts side (F). KiCad 10 |
| `GPSCard2_BOM.csv` | Parts list (hand assembly) |
| `fab/` | Gerbers and drill files (`GPSCard2_gerbers.zip`) for a bare-board order. The placement files are empty: nothing is machine-placed |
| `generate/` | Scripts that produced the files, copied from `GPSCard/generate/` and adapted. Stock KiCad symbols and footprints only |

## Signal paths

```
J2 pin 2 (TX, to the puck's RX)   ── R5 100 R ── RC6  U1TX by PPS  (U1 pin 17)
J2 pin 3 (RX, from the puck's TX) ── R6 100 R ── RC7  U1RX         (U1 pin 18)
JP1 (shunt 1-2 = +3V3, 2-3 = +5V) ── F1 0.25 A PTC ── J2 pin 1 (puck power, C8 1 uF)
Slot CANH / CANL (5 / 6) ── U2 MCP2562 ── TXD ◄ RB2 CANTX by PPS (pin 23),  RXD ── R4 1k ──► RB3 CANRX (pin 24)
Slot ID (11): R1 10k to 3.3 V here, the slot's resistor to GND on the main board ──► RA1 ANA1 (pin 3)
Slot +5 V (1 / 2) ── U3 MCP1700-3.3 ── +3V3 (MCU, MCP2562 VIO, a 3.3 V puck)
```

### MCU pins (PIC18F26Q83, SPDIP-28)

From Microchip's data sheet DS40002253 (pin allocation table, PPS input / output tables). On 28-pin parts the PPS
can put UART1, UART2 and CAN on ports B and C only.

| Pin | Port | Function | To |
| --- | --- | --- | --- |
| 1 | RE3 | MCLR / VPP | J3 pin 1, R2 10k to 3.3 V (no capacitor: the programmer drives MCLR) |
| 3 | RA1 | ANA1 | slot ID |
| 8, 19 | VSS | GND | |
| 9 / 10 | RA7 / RA6 | OSC1 / OSC2 | Y1 16 MHz, C4 / C5 27 pF |
| 13 | RC2 | GPIO | LED D1 (high = on, R3 1k) |
| 17 | RC6 | U1TX (PPS) | puck (J2 pin 2) |
| 18 | RC7 | U1RX (POR default) | puck (J2 pin 3) |
| 20 | VDD | 3.3 V | C3 100 nF |
| 23 | RB2 | CANTX (PPS) | MCP2562 TXD |
| 24 | RB3 | CANRX (POR default) | MCP2562 RXD (through R4 1k) |
| 25 | RB4 | U2TX (PPS) | J4 pin 2 |
| 26 | RB5 | U2RX (PPS) | J4 pin 3 |
| 27 | RB6 | ICSPCLK | J3 pin 5 |
| 28 | RB7 | ICSPDAT | J3 pin 4 |

All other pins are open: set them to outputs driven low, or analog, in firmware. Nothing else sits on RB6 / RB7 (the
data sheet says no pull-ups or capacitors on the ICSP pins).

| Connector | Pins |
| --- | --- |
| J1, slot (2×15, right angle, bottom edge) | As rev A. Used: 1 / 2 +5 V, 3 / 4 / 7 / 19 / 20 GND, 5 CANH, 6 CANL, 11 slot ID |
| J2, GPS puck (JST PH S4B-PH-K, top edge) | As rev A: 1 puck power, 2 TX (out), 3 RX (in), 4 GND. Pad 1 on the right, seen from the parts side |
| J3, ICSP (1×6, 2.54 mm, right angle, left edge) | 1 MCLR/VPP, 2 VDD (3.3 V), 3 GND, 4 ICSPDAT, 5 ICSPCLK, 6 not used. Pin 1 is the lowest pin. This is the PICkit / MPLAB Snap pinout: the programmer's pin 1 arrow goes to the lowest pin |
| J4, debug serial (1×3, 2.54 mm, right angle, left edge, below J3) | 1 GND, 2 TX (out of the card), 3 RX (into the card); UART2, 3.3 V. Pin 1 is the lowest pin |
| JP1, puck supply (1×3 header + shunt) | Shunt on 1-2 (left, silk "3V3") = 3.3 V puck; on 2-3 (right, "5V") = 5 V puck |

## Board

- **Outline:** 80 × 60 mm, no mounting holes, the slot and the case hold it (as rev A).
- **J1** is placed exactly as on rev A and the audio card (`gen_pcb.py` checks its orientation from the pads). **J2** is
  centred on the top edge at x 40. **J3 / J4** are right angle on the left edge, their pins standing about 6.5 mm past it.
- **U1** lies along the board in the middle-left, pin 1 at the left end beside J3, so MCLR and the ICSP pins (1, 27,
  28) run straight to the header. Y1 sits right below OSC1 / OSC2 with its load caps beside it. **U2** sits above the
  slot's CAN pins, **U3** beside its 5 V pins.
- **Every reference is on the silk** (inside the body where it fits, otherwise beside it), with the pin legends of J2,
  J3, J4 and JP1.
- **Layers:** 2. F carries most of the routing and a GND pour; B is the ground reference, a GND pour with a little
  routing. GND is not routed: every GND pin reaches both pours through its own hole. 0.25 mm tracks, 0.4 mm on the
  supplies, 0.2 mm clearance, 0.6 / 0.3 mm vias, ground stitching on a 4 mm grid.

## Status

**Schematic: done** (25 parts). ERC: no errors (warnings are off-grid pins of the stock symbols and the `GPSCard2`
library nickname, as rev A). **PCB: placed and routed**, DRC with the schematic parity check: 0 errors, 0 unconnected;
the only warnings are the right-angle headers' silk past the board edge, on purpose. Not ordered, not built,
**no firmware yet**.

### Check before ordering

- **PIC pins: checked** against the Q83 data sheet online (DS40002253, Microchip's online docs): the SPDIP-28 pin
  allocation table, the PPS tables (U1RX default RC7, U2RX default RB7, CANRX default RB3; UART1 / UART2 / CAN in and
  out on ports B or C on 28-pin parts), the HS crystal limit (20 MHz above 2.5 V), the PLL input range (4-16 MHz, out
  16-64 MHz) and VDD 1.8-5.5 V. KiCad has no Q83 symbol: the PIC18F26K83 SPDIP symbol is used, and its pins match the
  Q83 table pin for pin.
- **MCP1700 TO-92 pinout: from KiCad's MCP1700 symbol** (1 GND, 2 VIN, 3 VOUT), not yet from Microchip's data sheet
  (its download was blocked). Check it, and the flat-face direction on the silk, before soldering U3.
- **Crystal load caps:** 27 pF suits an 18 pF-load crystal with about 5 pF of stray capacitance. With a crystal of
  another load, use 2 × (CL − 5 pF).
- **F1:** Bel Fuse 0ZRE0025FF (0.25 A hold) because KiCad has its footprint. Any radial PTC around 0.2-0.3 A hold with
  5.1 mm lead spacing fits.
- **Radial capacitors:** footprints are for 2.54 mm lead spacing (KEMET C320-size parts). 10 µF must be ceramic (the
  MCP1700 is specified with ceramic capacitors).
- **Still by eye** (as rev A): J2 against the case panel cut-out, J3 / J4 against the neighbouring card, the outline
  against the case. The socketed DIP-28 stands about 8 mm tall, well under the 20 mm limit.

## Firmware notes (not written yet)

- **Toolchain:** MPLAB X with XC8, or MCC (MPLAB Code Configurator) for the PPS, CAN FD and UART setup.
- **Config bits:** `RSTOSC = EXTOSC with 4x PLL`, `FEXTOSC = HS` (16 MHz crystal × 4 = 64 MHz), **`JTAGEN = OFF`** (JTAG
  shares RA0, RA5, RB3 / CANRX and RB5 / U2RX), `LVP = ON` (the default) so a PICkit or Snap programs it at 3.3 V with
  no high voltage on MCLR, `MCLRE` on.
- **PPS:** `RC6PPS = U1TX`, `U1RXPPS = RC7`; `RB4PPS = U2TX`, `U2RXPPS = RB5`; `RB2PPS = CANTX`, `CANRXPPS = RB3`.
  Clear `ANSELB` / `ANSELC` bits on the digital pins; keep RA1 analog.
- **CAN:** classic CAN 2.0B at 500 kbit/s (`docs/hardware/accessory_ports.md`), clocked from FOSC (64 MHz) or the
  16 MHz crystal. Node `0x14 + (slot − 1)`, IDs `0x222`-`0x226`, heartbeat `0x700 + node`, NET ANNOUNCE product
  `0x0005`, as `docs/hardware/gps_card.md`.
- **Puck UART:** auto-baud 9600 / 38400 / 115200 as the spec says. At 64 MHz with BRGH, 115200 baud has under 0.1 %
  error. Turn on RC7's weak pull-up (`WPUC7`) so the input does not float with no puck.
- **Slot ID:** read RA1 at boot (10k up here, 1k-47k down on the main board: about 0.30-2.72 V).
- **LED (RC2, high = on):** off = nothing from the puck, blinking = searching, steady = 3D fix.
- **Programming:** power the card from the slot (or a bench 5 V on J1 pins 1 / 2) and plug the programmer into J3,
  with the programmer's "power target" option off. J3 pin 2 is only there so the programmer can sense 3.3 V: powering
  the card through it would feed U3's output with no input, which nobody has checked.

## Assembly order

Lowest parts first: resistors, the crystal, ceramic capacitors, the LED (flat side = square pad), the PTC, the DIP-28
and DIP-8 sockets (notch as the silk), U3, the JST connector, then the pin headers (J3, J4 right angle on the left
edge, JP1 straight, J1 last). Fit JP1's shunt for the puck you have, then plug in U1 and U2.

## Regenerating

As rev A (KiCad 10, numpy in KiCad's python, Freerouting 1.9.0 with Java 17+):

```bash
KIPY=/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3
python3 generate/gen_sch.py                       # also writes generate/refs.json
kicad-cli sch erc GPSCard2.kicad_sch              # expect warnings only (off-grid, library nickname)
JAVA=/opt/homebrew/opt/openjdk@17/bin/java $KIPY generate/gen_pcb.py --route "$PWD/../DigiDash2/generate/freerouting-1.9.0.jar"
python3 generate/drc.py                           # DRC with the schematic parity check
python3 ../tools/fab_export.py GPSCard2           # Gerbers + drill into fab/
```

Give Freerouting's jar as an absolute path: the router runs from `$TMPDIR`. Temporary files go to `$TMPDIR` with a
`gpscard2` prefix.
