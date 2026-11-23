# Radio card, rev B (through-hole)

The hand-assembled version of the [radio card](../RadioCard/README.md). Same job, same **80 × 60 mm slot card**, same
slot use (CAN, the I2S rail's BCK / WS in and RET out) and CAN IDs (`docs/hardware/radio_card.md`). The difference is
that **every part is through-hole**, so you can solder the whole board yourself, and the tuner is a plug-in module:

- the owner's **Si4737-C40-GU breakout** (FMBreakout.brd, OSH Park, 22.4 × 29.4 mm) in a **1×8 and a 1×9 socket**
  (J6 / J7), in place of rev A's Si4735-D60 (a DigiKey-only SSOP that had to be machine-placed). The Si4737 is AM / FM /
  weather band + RDS with digital audio out; it has no SW / LW, which this card never used
- a **PIC18F26Q83-I/SP** (SPDIP-28, in a socket), as [GPSCard2](../GPSCard2/README.md): PIC18 at 64 MHz with its own
  **CAN FD controller** (run as classic CAN at 500 kbit/s), I2C and 5 UARTs, with a 16 MHz HC-49 crystal (4x PLL)
- an **MCP2562-E/P** CAN transceiver (DIP-8; VDD 5 V, VIO 3.3 V, STBY low, no termination)
- a **74HC125** (DIP-14) as the RET buffer, off at reset, and an **MCP1700-3302E/TO** 3.3 V regulator (TO-92)
- the **edge-mount SMA** of rev A (Amphenol 132289, already on hand; the one part soldered by hand on both faces of the
  board edge) and the AM loopstick header J5 on the top edge
- **J3, a 6-pin Microchip ICSP header** and **J4** debug serial, both right angle on the left edge; two 3 mm LEDs
- axial 1/4 W resistors (10.16 mm pitch), radial capacitors (2.54 mm), an axial ferrite bead and an axial 56 nH inductor

**Antenna ESD: D2 is not fitted, D4 / D5 and SG1 stand in for it.** The TPD1E0B04 (0.13 pF) has no through-hole version,
and nothing through-hole comes close to AN383's ≤ 1 pF. In its place: **SG1**, a 0.25 mm PCB spark gap (bare copper, nothing
to fit) right under the SMA's centre pin, which takes the bulk of a big static hit, and **D4 / D5**, two 1N5711 Schottky
diodes back to back from the antenna node to ground, which clamp what gets past it to about ±0.4 V (the tuner input's
limit). The cost is about 4 pF on the antenna node: some FM signal, and L1 has to be tuned with them in (see
[Tuning L1](#tuning-l1)). If reception suffers, compare with them lifted. D2's 0402 pads stay on the board.

| Files | |
| --- | --- |
| `RadioCard2.kicad_sch` / `.kicad_pcb` / `.kicad_pro` | The card: 80 × 60 mm, 2 layers, every part on the parts side (F). KiCad 10 |
| `RadioCard2_BOM.csv` | Parts list (hand assembly) |
| `fab/` | Gerbers and drill files (`RadioCard2_gerbers.zip`) for a bare-board order. Nothing is machine-placed: the placement files list only J2, the edge-mount SMA, which you solder yourself |
| `generate/` | Scripts that produced the files: `gen_sch.py` new (rev A's circuit on GPSCard2's parts), `gen_pcb.py` from `GPSCard2/generate/` with rev A's RF keep-out. Stock KiCad symbols and footprints only |

## Signal paths

```
Slot BCK (15) ── R8 2k ── J6 pin 3  P3 = GPO3 / DCLK       (also the tuner's reference clock: RCLKSEL = DCLK)
Slot WS  (16) ── R7 2k ── J6 pin 2  DFS                    (C10 2.2 pF to GND on the WS side; R9 / R10 100k idle pull-downs)
J6 pin 1 DOUT ── R6 604 ── U5 74HC125 buffer A (OE = RA4, 10k up = off) ── R15 33 ── Slot RET (18)
Slot MCLK (24) ── R12 0 R (NOT FITTED) ── J7 pin 4 RCLK (R13 100k to GND)
SMA J2 ── SG1 gap to GND ── D4 / D5 1N5711 pair to GND ── L1 56 nH to GND ── C12 1 nF C0G ── J6 pin 6 FMI      J6 pin 7 RFG = GND
J5 (loopstick) ── C13 470 nF ── J6 pin 8 AMI                       JP2 (no shunt) joins the SMA node to the AM node
+3V3 ── FB1 ── +3V3A ── J7 pin 3 (3.3V, VA and VD on the breakout), C3 10 uF + C4 100 nF at the pin
PIC RC3 / RC4 (SCL1 / SDA1) ── J7 pins 6 / 5 SCLK / SDIO   (pull-ups: the breakout's own 10k to its 3.3 V)
PIC RA2 ── J7 pin 8 RST (R11 100k to GND)    PIC RB0 ── J6 pin 4 P2 = GPO2 / INT    JP1 ── J7 pin 7 SEN
Slot CANH / CANL (5 / 6) ── U2 MCP2562 ── TXD ◄ RB2 CANTX by PPS (pin 23),  RXD ── R5 1k ──► RB3 CANRX (pin 24)
Slot ID (11): R1 10k to 3.3 V here, the slot's resistor to GND on the main board ──► RA1 ANA1 (pin 3)
Slot +5 V (1 / 2) ── U3 MCP1700-3.3 ── +3V3 (PIC, MCP2562 VIO, 74HC125, FB1)
```

### The breakout's pins (J6 / J7)

As the breakout's own silk names them, each header counted from the **headphone-jack end** (socket pin 1). The breakout
sits **upside down** on the card: jack end toward the slot (silk "jack end"), so its FMI / AMI pins are nearest the SMA.

| Socket pin | J6 (breakout left header) | Si4737 pin | J7 (breakout right header) | Si4737 pin |
| --- | --- | --- | --- | --- |
| 1 | DOUT | 1 | LOUT (open) | 24 |
| 2 | DFS | 2 | ROUT (open) | 23 |
| 3 | P3 = GPO3 / DCLK | 3 | 3.3V (VA + VD) | 20, 21 |
| 4 | P2 = GPO2 / INT | 4 | RCLK | 19 |
| 5 | P1 = GPO1 (open: high or open at reset) | 5 | SDIO | 18 |
| 6 | FMI | 8 | SCLK | 17 |
| 7 | RFG (RF ground) | 9 | SEN | 16 |
| 8 | AMI | 12 | RST | 15 |
| 9 | | | GND | 13, 14 |

On the breakout itself: the DBYP capacitor (pin 22), 10k pull-ups on SDIO / SCLK and the headphone jack on LOUT / ROUT.
It has **no crystal** (the reference comes from DCLK), **no supply decoupling** (C3 / C4 here) and **no antenna coupling
capacitors** (C12 / C13 here).

### MCU pins (PIC18F26Q83, SPDIP-28)

| Pin | Port | Function | To |
| --- | --- | --- | --- |
| 1 | RE3 | MCLR / VPP | J3 pin 1, R2 10k to 3.3 V |
| 3 | RA1 | ANA1 | slot ID |
| 4 | RA2 | GPIO out | tuner RST (R11 100k to GND holds it in reset until the firmware runs) |
| 6 | RA4 | GPIO out | RET buffer OE, active low (R14 10k up = buffer off at reset and on a blank chip) |
| 8, 19 | VSS | GND | |
| 9 / 10 | RA7 / RA6 | OSC1 / OSC2 | Y1 16 MHz, C6 / C7 27 pF |
| 12 | RC1 | GPIO | LED D1 TUNED (high = on, R3 1k) |
| 13 | RC2 | GPIO | LED D3 STEREO / RDS (high = on, R4 1k) |
| 14 | RC3 | SCL1 | tuner SCLK |
| 15 | RC4 | SDA1 | tuner SDIO |
| 20 | VDD | 3.3 V | C5 100 nF |
| 21 | RB0 | INT0 | tuner GPO2 / INT |
| 23 | RB2 | CANTX (PPS) | MCP2562 TXD |
| 24 | RB3 | CANRX (POR default) | MCP2562 RXD (through R5 1k) |
| 25 | RB4 | U2TX (PPS) | J4 pin 2 |
| 26 | RB5 | U2RX (PPS) | J4 pin 3 |
| 27 | RB6 | ICSPCLK | J3 pin 5 |
| 28 | RB7 | ICSPDAT | J3 pin 4 |

All other pins are open: set them to outputs driven low, or analog, in firmware.

| Connector | Pins |
| --- | --- |
| J1, slot (2×15, right angle, bottom edge) | As rev A. Used: 1 / 2 +5 V, 3 / 4 / 7 / 19 / 20 GND, 5 CANH, 6 CANL, 11 slot ID, 15 BCK, 16 WS, 18 RET, 24 MCLK (only through R12, not fitted) |
| J2, antenna (SMA, edge mount, top edge) | Centre = FM antenna node, legs = GND (both faces) |
| J3, ICSP (1×6, right angle, left edge) | 1 MCLR/VPP, 2 VDD (3.3 V), 3 GND, 4 ICSPDAT, 5 ICSPCLK, 6 not used. Pin 1 is the lowest pin (PICkit / Snap pinout) |
| J4, debug serial (1×3, right angle, left edge) | 1 GND, 2 TX (out of the card), 3 RX (into the card); UART2, 3.3 V. Pin 1 is the lowest pin |
| J5, AM loopstick (1×2, right angle, top edge) | 1 AM node, 2 GND |
| J6 / J7, tuner breakout (1×8 / 1×9 female, 17.78 mm apart) | See the table above |
| JP1, tuner address (1×3 + shunt) | 1-2 = SEN low = I2C 0x11 (the firmware default); 2-3 = 0x63 |
| JP2, AM from whip (1×2, no shunt) | A shunt feeds the SMA to the AM input too (needs a car-whip AM network that AN383 does not give) |

## Board

- **Outline:** 80 × 60 mm, no mounting holes, the slot and the case hold it (as rev A).
- **J1** is placed exactly as on rev A and GPSCard2. **U1** lies along the board in the middle-left with Y1 under it,
  ICSP and debug on the left edge, as GPSCard2. **The breakout** sits at the top right (x 52-74, y 9-39) on its two
  sockets, jack end toward the slot: FMI / AMI at its top, near **J2** (SMA, x 70) and **J5** (x 58); DOUT / DFS / DCLK
  at its bottom, toward **U5** and the rail pins. **U2** sits above the slot's CAN pins, **U3** beside its 5 V pins.
- **RF keep-out:** no tracks on B in the top-right corner (x 60-80, y 0-23: the SMA, L1, the coupling caps and the
  breakout's RF pins), so the B ground pour is solid under the front end (as rev A).
- **Under the breakout:** nothing is placed in its outline, so it plugs in flat. The breakout's own headers and jack
  pins stand below its board; the sockets give it 8.5 mm.
- **Every reference is on the silk**, with the pin legends of J3, J4, J5 and the breakout's pin names beside J6 / J7.
- **Layers:** 2, as GPSCard2: GND pours on both, 0.25 mm tracks, 0.4 mm supplies, 0.2 mm clearance, ground stitching on a 4 mm grid.

## Status

**Schematic: done** (50 parts; D2 and R12 not fitted, SG1 is copper only). ERC: no errors (warnings are off-grid pins of the stock symbols and the
`RadioCard2` library nickname, as GPSCard2). **PCB: placed and routed**, DRC with the schematic parity check: 0 errors,
0 unconnected; the warnings are silkscreen only (right-angle headers past the board edge on purpose, a few references
touching neighbours), plus J2's 5 copper-to-edge hits, expected for an edge-launch SMA (`drc.py` lists them apart, as rev A). Not ordered, not built, **no firmware yet**. The breakout itself has not been powered yet.

### Check before ordering

- **The breakout first.** Before ordering this board, prove the module on the bench: continuity from each header pin to
  the chip pin in the table above, no solder bridges on the SSOP, then I2C POWER_UP and GET_REV (part number 37). If its
  headers turn out to be in another order, only J6 / J7's pin assignments in `gen_sch.py` change.
- **Breakout geometry:** 17.78 mm between its headers, the tops of both headers in line, the 1×9 one pin longer at the
  far end (measured from its OSH Park render, not from the board). Check with calipers, and the 22.4 × 29.4 mm outline.
- **Height:** socket 8.5 mm + the breakout's header spacer 2.5 mm + its board 1.6 mm + the headphone jack about 5 mm =
  about 17.5 mm, just under the 20 mm limit. Measure the jack. The SMA sits on the top edge as on rev A.
- **PIC I2C pins: not checked yet.** RC3 / RC4 as SCL1 / SDA1 are the usual default I2C pins on 28-pin PIC18 parts;
  confirm them in the Q83 data sheet's PPS table (DS40002253) before ordering.
- **MCP1700 TO-92 pinout and crystal load caps:** as GPSCard2.
- **L1:** 56 nH axial parts are rare; the footprint takes any axial part with 7.62 mm lead spacing, or a hand-wound coil.
- **SG1** is a project footprint (`RadioCard2.pretty`, two pointed pads 0.25 mm apart, mask open): check on the fab's
  preview that the gap and the mask opening survive. 0.25 mm is above the 0.2 mm clearance rule, so any fab can make it.

## Tuning L1

**What L1 is:** a small coil, the size of a 1/4 W resistor. An axial RF inductor looks like a resistor with a green or blue
body and colour bands; a hand-wound one is a few turns of enamelled wire, like a tiny spring. It sits on the board between
the antenna node and ground, beside the breakout (silk L1).

**Why it needs tuning:** to a radio, a whip and its cable behave mostly like a small capacitor. L1 and that capacitance
make a resonant circuit, and when its resonance falls in the FM band (88-108 MHz) the antenna hands the most signal to the
tuner. AN383 picked 56 nH for its test whip, but your car's whip, its cable length and D4 / D5's 4 pF all add capacitance,
so the best value on the car may be different. More capacitance (a longer cable, the diodes) wants a **smaller** L1.
"Tuning" just means trying a few values and keeping the one that receives best. Nothing is adjusted with a screwdriver.

**How:**
1. Fit L1 in two turned-pin socket contacts (BOM) so a value swaps in seconds, with the card unpowered each time.
2. Use the car's own whip and cable, connected as they will be in the car (the car parked, the card on the bench supply or
   in the dash). A bench whip proves nothing about the car.
3. Tune to two or three stations, one weak, and read the tuner's signal strength (RSSI) and signal-to-noise (SNR): the
   firmware prints them on the debug serial port (FM_RSQ_STATUS). Write them down.
4. Try 56 nH, then a smaller (33 or 39 nH) and a larger (82 or 100 nH) value. Keep the one with the best SNR on the weak
   station. If two values are close, keep the smaller.
5. Try with L1 pulled out too, for comparison, but **put one back**: L1 is also the DC path that drains static off the whip.

Expect a few dB between values, not a night-and-day change: a car whip is a decent antenna either way.

## Firmware notes (not written yet)

As GPSCard2 (config bits, PPS, CAN at 500 kbit/s, slot ID on RA1, programming over J3), plus:

- **I2C:** I2C1 on RC3 / RC4, host mode, 100 or 400 kHz. Tuner address 0x11 (JP1 1-2).
- **Tuner bring-up:** as `docs/hardware/radio_card.md` (rev A), with the Si4737 in place of the Si4735: RST low, wait for
  the rail's clocks, RST high with SCLK high (2-wire mode), POWER_UP with digital output (OPMODE 0xB0), REFCLK from DCLK
  (`REFCLK_PRESCALE` 0x1060, `REFCLK_FREQ` 32000), then the sample rate. Bands: FM and AM only.
- **RET buffer:** RA4 low only after a NET RAIL grant; back high within 10 ms of a withdrawal.
- **Node:** product 0x0009, node `0x14 + (slot − 1)`, as rev A.
- **Tuning aid:** print RSSI and SNR (FM_RSQ_STATUS) on the debug serial once a second while tuned, for [Tuning L1](#tuning-l1).

## Assembly order

Lowest parts first: resistors, L1 (or its two socket contacts), FB1, D4 / D5 (standing, band to K), the crystal, ceramic capacitors, the LEDs (flat side = square pad), the DIP
sockets and DIPs (U5, U2, the DIP-28 socket; notches as the silk), U3, JP1 / JP2, the SMA (centre pin and top legs first,
then the bottom legs), J6 / J7 (sockets, vertical),
then the right-angle headers (J3, J4, J5, J1 last). Fit JP1's shunt on 1-2, plug in U1, then the breakout, jack end
toward the slot.

## Regenerating

As GPSCard2 (KiCad 10, numpy in KiCad's python, Freerouting 1.9.0 with Java 17+):

```bash
KIPY=/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3
python3 generate/gen_sch.py                       # also writes generate/refs.json and netlist.json
kicad-cli sch erc RadioCard2.kicad_sch            # expect warnings only (off-grid, library nickname)
JAVA=/opt/homebrew/opt/openjdk@17/bin/java $KIPY generate/gen_pcb.py --route "$PWD/../DigiDash2/generate/freerouting-1.9.0.jar"
python3 generate/drc.py                           # DRC with the schematic parity check
python3 ../tools/fab_export.py RadioCard2         # Gerbers + drill into fab/
```
