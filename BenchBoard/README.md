# DigiDash bench board

A small board that stands in for DigiDash2 on the bench: **two card slots and one round screen on one CAN bus**,
powered from a 12 V bench adapter, with a header for a USB-CAN adapter. A card, a screen, or both talk to each other
exactly as they would in the dash, without ordering the 309 × 104 mm 4-layer main board.

- **100 × 100 mm, 2 layers**, every part on the top side (the cheapest prototype bracket at JLCPCB / PCBWay)
- **Two slots** (slot 1 and slot 2 of DigiDash2: same 2×15 socket, same pinout, same 25.4 mm spacing, parallel; cards
  stand on edge with their parts side toward the top edge, slot 1 at the open end for a card with tall parts)
- **One screen** on a right-angle 2×20 header at the bottom edge: the Waveshare ESP32-P4-WIFI6-Touch-LCD-4C plugs on
  and stands up, facing away from the board. A **ROLE jumper** sets which screen it is, by the same strap as DigiDash2

| Files | |
| --- | --- |
| `BenchBoard.kicad_sch` / `.kicad_pcb` / `.kicad_pro` | The board. KiCad 10 |
| `BenchBoard_BOM.csv` | Parts list, every part with maker and part number (`python3 generate/gen_bom.py`, which also writes `fab/BenchBoard_BOM_PCBWay.csv`) |
| `fab/` | Gerbers, drill, placement, centroid (`python3 ../tools/fab_export.py BenchBoard`) and the assembler's BOM |
| `generate/` | `gen_sch.py`, `gen_pcb.py`, `gen_bom.py`, `drc.py`, `sexp.py` (`refs.json` is written by `gen_sch.py`). The GPS card's 2-layer generators, adapted |

**Firmware:** none on the board itself. The screen runs [`firmware/esp32_p4_display`](../firmware/esp32_p4_display/); each card runs its own (see [`HARDWARE.md`](../HARDWARE.md)).

## Connectors, jumpers, LEDs

| Ref | Silk | What it is |
| --- | --- | --- |
| J1 | 12V IN | 5.5 / 2.1 mm barrel jack, centre positive. A **12 V, 3 A** adapter; **17 V at most** (the buck's limit). SS34 reverse-polarity diode behind it |
| J7 | CAN H L G | USB-CAN adapter |
| J8 | TERM | Fitted = 120 Ω across the bus here. Terminate the adapter's end too |
| J2 | IGN | Fitted = key on: switched 12 V to the slots (pin 8). Pull it to test a card's key-off behaviour |
| J5 | LAMP | Fitted = dash lights on: the slots' LAMP level (pin 13) goes to 3.3 V and the screen's illumination pin (GPIO24) goes low (dim) |
| J6 | REV | Fitted = in reverse: the slots' REV level (pin 14) goes to 3.3 V and the screen's reverse trigger (GPIO25) goes low |
| D3 | WAKE | Lit while a card holds the slots' WAKE line high (the sensor card does after key-off) |
| D2 | PWR | 5 V present |
| J3, J4 | SLOT 1, SLOT 2 | Card slots (2×15 sockets, DigiDash2's part). Slot IDs 1k and 3.3k: the cards come up as nodes 0x14 and 0x15 |
| J9 | SCREEN | The screen's 40-pin socket plugs onto this right-angle header |
| J10 | ROLE L C R | Jumper 1-2 = **left**, 2-3 = **right**, none = **center** (GPIO4 strap: 1k to GND, 1k to the screen's 3V3, or open, as DigiDash2) |
| J11 | SENS | Sensor serial in: a jumper wire from the sensor card's J8 (pin 2 TX, pin 1 GND). 10k / 20k divider to the screen's GPIO5 |
| J12 | 3V3 TX RX G | Serial port for a GPS puck, JST PH 4 pin, DigiDash2 J6's pinout: TX out = screen GPIO3, RX in = screen GPIO2 (the firmware's `GPS_UART_TX` / `_RX`) |
| J13 | SCL SDA G | Cluster I2C (screen GPIO48 / GPIO47, 4.7k pull-ups): to a second screen or a probe |
| J14 | BCK WS MIC RET MCLK G | I2S rail probe (slot pins 15–18, 24), bused between the slots as on DigiDash2 |
| J15 | AUDIO | 3.5 mm stereo jack on the slots' audio bus (21 L, 22 R, 23 audio GND) |

Power: TPS563201 buck to 5 V / 3 A (TI's typical application, Table 7-2 values for 5 V: 54.9k / 10k, 3.3 µH,
2 × 22 µF), AP2112K-3.3 for 3.3 V. Per slot, as DigiDash2: 0.5 A PTC on 5 V and switched 12 V, 0.1 A on 3.3 V,
0.2 A on constant 12 V; AUX 12 V to both slots through one 1.5 A PTC (24 V-rated 1812); AUX GND and audio GND meet
GND only at the net ties NT1 / NT2. The screen has its own MCP2562 (VDD 5 V, VIO 3.3 V, 10k on TXD), as on DigiDash2.

## Mechanical: stand it on 45 mm standoffs

The screen's 40-pin socket is low on its back board, so when it stands up on J9 it hangs **about 30–40 mm below the
header pins**. Put the board on **four M3 standoffs, 45 mm** (holes H1–H4 at the corners), or at the edge of the bench
with the screen hanging over. The screen faces away from the board: look at it from beyond the bottom edge. Cards
stand above the slots, about 60 mm tall plus the socket.

Don't plug the screen's own USB-C in while the board powers it: both would drive its 5 V (DigiDash2 has the same rule).

## The screen header's pin mapping

The Waveshare board numbers its 40-pin socket like a Raspberry Pi header (pin 1 = 3V3, pin 2 = 5V, read off its
silkscreen). Standing up on a right-angle header, the socket meets the header with its columns reversed: **J9 pad p
meets screen pin 40 − p (odd p) or 42 − p (even p)**. `gen_sch.py` wires the pads that way (the `WS` table), and the
pin-by-pin check below confirmed it on the board. DigiDash2's headers have their own mirror (the screens sit face down
there); both reach the same screen pins.

| Screen pin | GPIO | Bench board | | Screen pin | GPIO | Bench board |
| --- | --- | --- | --- | --- | --- | --- |
| 2, 4 | 5V | +5V | | 24 | 36 | CAN RX (MCP2562 RXD) |
| 6 9 14 20 25 30 34 39 | GND | GND | | 32 | 34 | CAN TX (MCP2562 TXD) |
| 17 | 3V3 | ROLE "R" strap only | | 27 | 24 | LAMP (low = dim) |
| 18 | 4 | ROLE strap | | 28 | 25 | REV (low = reverse) |
| 16 | 5 | SENS (sensor serial in) | | 33 | 48 | cluster I2C SCL |
| 19 | 3 | serial TX out | | 37 | 47 | cluster I2C SDA |
| 21 | 2 | serial RX in | | others | | not connected (the video pins too) |

## Status

**Schematic, board and BOM: done** (2026-10-05). Nothing ordered.

- ERC: no errors (off-grid and symbol-library warnings, as every generated board here). Every pin of every part
  checked against the nets `gen_sch.py` meant (262 pins, 0 mismatches).
- PCB: placed and routed by Freerouting in one round, complete. DRC with the schematic parity check: **0 errors,
  0 unconnected**; warnings: silkscreen past the board edge (the connectors that stand past it on purpose) and J15's
  footprint differing from the library (the generator removes the marks the stock footprint draws on Edge.Cuts).
- No via touches a pad. Every GND pad reaches the B pour.
- J9's pads checked one by one against the Waveshare pin each one meets (from the pad positions, not the schematic):
  0 mismatches.
- Slot sockets: placed so a card's pin n meets socket pad n with its parts side toward the top edge (the generator
  checks the orientation and stops if it is wrong). Pinout and slot-ID values from DigiDash2's netlist.

### Check before ordering

- **The screen's back board, in hand:** that J9's pins reach its socket with the screen upright (the socket at the
  bottom of its back board, as in Waveshare's drawing), and how far it hangs below the board (for the standoffs).
- **The gauge face's orientation** on the bench: the screen stands with its header at the bottom; if the faces come
  out rotated compared with the dash, rotate them in firmware.
- The 12 V adapter: 3 A, and no more than 17 V unloaded.

## Found while designing this (DigiDash2, not this board)

- **DigiDash2's J6 serial port has TX and RX swapped** against the firmware. The firmware uses GPIO2 as RX and GPIO3 as
  TX (`GPS_UART_RX` / `_TX`); DigiDash2 wires J6 pin 2 "TX out" to the screen's GPIO2 and pin 3 "RX in" to GPIO3
  (J1 pins 22 / 20 = Waveshare pins 21 / 19). This board wires them the firmware's way.
- **DigiDash2's 0.5 A PTCs on the slots' switched 12 V** are MF-NSMF050-2, rated 13.2 V; a running car is about 14.4 V.
  This board uses Littelfuse 1206L050/15YR (15 V) there.

## Regenerating

```bash
KIPY=/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3
python3 generate/gen_sch.py                       # also writes generate/refs.json (groups and intended nets)
kicad-cli sch erc BenchBoard.kicad_sch            # expect warnings only
$KIPY generate/gen_pcb.py --route freerouting-1.9.0.jar   # place, route (Freerouting 1.9.0, Java 17+), fill, stitch
python3 generate/drc.py                           # DRC with the schematic parity check
python3 generate/gen_bom.py                       # both BOMs
python3 ../tools/fab_export.py BenchBoard         # Gerbers, drill, placement
```
