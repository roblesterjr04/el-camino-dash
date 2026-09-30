# DigiDash main board, rev 2

The dash's main board, redrawn per [`docs/hardware/digidash_rev2_plan.md`](../docs/hardware/digidash_rev2_plan.md):

- all SMD except connectors, on both sides: the 2560 core, video decoder and screen transceivers on the front (under
  the screens), power, AUX 12 V, sensor inputs and every connector on the back
- an ATmega2560 sensor pack on the CAN bus, with the UART fan-out kept as a fallback
- the TVP5150AM1 video decoder and the FRAM on the board
- two double accessory bays with their own AUX 12 V power
- four cable ports and a trunk port

Rev 1 stays in [`DigiDash/`](../DigiDash) as the reference.

| Files | |
| --- | --- |
| `DigiDash2.kicad_sch` / `.kicad_pcb` / `.kicad_pro` | The board: 309.2 × 103.7 mm (rev 1's outline), 4 layers. KiCad 10 |
| `DigiDash2.pretty` | The RCA jack footprint (video input). Everything else is stock KiCad or `DigiDash_Accessory` |
| `generate/` | `gen_footprints.py`, `gen_sch.py`, `gen_pcb.py`, `drc.py` (see [Regenerating](#regenerating)) |

## What is on it

| Block | Parts | Notes |
| --- | --- | --- |
| Power in | J7 (Micro-Fit 3.0, 6 pin: 1–2 battery, 3–4 GND, 5 ignition, 6 illumination), Q2 reverse-polarity P-FET, SMCJ33A TVS | J7 carries about 10 A (two pins each for battery and ground) |
| 5 V | U2 TPS54560B, 5 A, 400 kHz: TI's typical application (datasheet figure 32) component for component | EN comes from the ignition through 442k / 90.9k (starts above about 6.5 V) with a 5.1 V clamp. The sensor pack's PWR_HOLD pin can keep it on after key-off, so it can finish its shutdown work |
| 3.3 V, 1.8 V | U11 AP7361C-33 (1 A), U12 AP2112K-1.8 | The board's own 3.3 V now feeds the CAN transceivers' VIO, the TVP5150's I/O, GPS and the I2C pull-ups (rev 1 borrowed the center screen's 3V3) |
| Switched 12 V | Q5 P-FET high-side switch, turned on by the ignition through Q4 | Feeds pin 8 of every bay position, the trunk port and the camera |
| AUX 12 V | J13 (Mini-Fit Jr, vertical), 10 A mini-blade fuse, Q6 reverse P-FET, TVS | Bay power on J14 / J15 (Micro-Fit 3.0). Its return, AUX_GND, meets GND only at net tie NT1 |
| Sensor pack | U1 ATmega2560-16AU, 16 MHz, 5 V; U7 MB85RC256V FRAM (0x50) | USART0 on J12 (FTDI, 5 V, DTR auto-reset). USART1 drives the sensor-frame fallback through one 10k / 20k divider per screen. Speed and tach on ICP5 / ICP4. Spare I/O on J26. ISP on J2 (use a USBasp or AVRISP mkII) |
| Sensor CAN | U3 MCP2515 (own 16 MHz crystal) + U6 MCP2562 | Node 0x04, no termination |
| Screens | J5 / J1 / J4 (same pin use as rev 1), U5 / U9 / U10 MCP2562 | 120 Ω at the left end, and at the right end through bridged jumper JP2 (cut it when the trunk port carries the bus on) |
| Sensor inputs | J3 (same pinout), J10 (same), J8 RPM select | As rev 1 in SMD, plus: a series resistor, clamp and filter on the speed input; a reverse diode across the tach optocoupler's LED; a 1k series resistor and 10 nF filter on each ADC input; a battery-voltage divider on ADC4 |
| Video | U8 TVP5150AM1 (address 0x5C), 14.31818 MHz crystal, J11 RCA input (37.4 Ω + 37.4 Ω termination, per TI) | Same J1 pins as rev 1's module. RESETB: RC delay, plus the center's GPIO52 (J1 pin 36) |
| Accessories | Bay 1: J18 (A) + J19 (B). Bay 2: J20 (A) + J21 (B). Cable ports J17, J22, J23, J24. Trunk J25 | See [`accessory_ports.md`](../docs/hardware/accessory_ports.md#rev-2-digidash2-bays) |

## Connectors (what each one is, and what the silkscreen says)

Every connector carries its name and pin labels on the silkscreen, on the side you plug into. Where the pin labels
cannot fit beside the pins, a legend line ("1 FUEL  2 ETEMP ...") stands in for them. Pin 1 is the square pad.

| Ref | Silk name | What it is | Pins (1, 2, ...) |
| --- | --- | --- | --- |
| J5 / J1 / J4 | LEFT / CENTER / RIGHT SCREEN | The three round screens (front, face down). Their labels are on the back too | as rev 1 |
| J7 | POWER IN | Vehicle power, Micro-Fit 3.0, 6 pin | 12V, 12V, GND, GND, IGN (key on), LAMP (dash lights) |
| J13 | AUX 12V IN | Separate high-current 12 V for the bays (own fuse at the fuse box) | 12V, GND |
| J3 | SENSORS | Senders, JST PH 8 pin (as rev 1) | FUEL, ETEMP, ETEMP V, ALT L, AMB, OILP V, OILP, GND |
| J10 | SPEED TACH | Hall speed sensor and coil, JST PH 4 pin (as rev 1) | SPD, TACH, 5V (hall feed), GND |
| J8 | RPM SEL | Pulses-per-revolution jumpers (as rev 1) | jumper A and / or B to GND |
| J9 | CAMERA | Backup camera and reverse trigger, JST PH 4 pin | REV (reverse lamp 12 V in), 12V (switched camera power), unused, GND |
| J11 | VIDEO IN | Camera's composite video, RCA | tip = video, sleeve = GND |
| J6 | GPS | GPS module, JST PH 4 pin | 3V3, G RX (to the module's RX), G TX (to the module's TX), GND |
| J17, J22, J23, J24 | CABLE 1 .. 4 | Cable ports for off-board CAN devices (console, nodes near a sensor) | 5V, H (CANH), L (CANL), G |
| J25 | TRUNK | Bus extension to the rear of the car, Micro-Fit 3.0 | H, L, 12V (switched), G. Cut jumper JP2 and terminate at the far end |
| J18 / J19 | BAY 1A / BAY 1B | Bay 1's two carrier positions (2x5 shrouded header) | see `docs/hardware/accessory_ports.md` |
| J20 / J21 | BAY 2A / BAY 2B | Bay 2's two carrier positions | as above |
| J14 / J15 | BAY 1 POWER / BAY 2 POWER | High-current power for a carrier, from J13, Micro-Fit 3.0 | 12V, 12V, GND, GND |
| J2 | AVR ISP | Program the ATmega2560 with an ISP programmer (USBasp or AVRISP mkII, not USBtiny) | MISO, 5V, SCK, MOSI, RST, GND |
| **J12** | FTDI 5V | **Serial port of the ATmega2560**, for reading its log output (and for uploading a bootloader-based sketch) with a 5 V FTDI or USB-serial cable. **Optional: you program the board through J2, so leave J12 unfitted until you want logs.** VCC is not connected: the board is powered from J7 | GND, CTS, (none), RXI, TXO, DTR |
| **J16** | CAN TAP | **A tap on the CAN bus** for a USB-CAN adapter, to watch the traffic while debugging | H (CANH), L (CANL), GND |
| **J26** | SPARE I/O | **Unused ATmega2560 pins** brought out for later (a new sensor input, a switch, a relay driver): 5 V, ground, analog A5–A8, and D44–D47 | 1 5V, 2 GND, 3 A5, 4 A6, 5 A7, 6 A8, 7 D44, 8 D45, 9 D46, 10 D47 |
| JP1 | DEMO | Close to make the sensor pack send a fake sweep (bench only) | |
| JP2 | TERM R | The bus's right-end 120 ohm, closed at the factory. Cut it if a trunk cable carries the bus on | |

Two jumpers are on the front of the board, under the left screen's area: **JP1 and JP2 are covered when the screens are
fitted.** Both are rarely-changed solder jumpers, but cutting JP2 for a trunk cable means taking the screen off. Say so
if you want them moved to the back (that means re-running placement and routing).

Notes on the labels: "G RX" and "G TX" are the GPS *module's* pin names. J3 "ETEMP V", "OILP V" and J10 "5V" are the
schematic's sender-feed nets (each is 5 V through its own 0.1 A PTC). The labels are added by `gen_pcb.py` (`--labels`
re-adds them to the current board) using free space beside each connector.

## Board

- **Layers:** F signal, In1 solid GND, In2 solid +5 V, B signal. Every SMD GND and +5 V pad has its own via to its plane.
  The F and B ground pours are stitched to In1.
- **Sides:** the buck and its loop, the AUX path, tall parts and all connectors stay on the back. The ICs of the
  sensor-pack core, the video decoder and the screen transceivers are on the front. Each passive goes on the side of
  the chip pin it connects to.
- **Rules:** 0.2 mm tracks and 0.13 mm clearance by default. 0.4 mm for 3.3 / 1.8 V, 0.8 mm for the 12 V nets, 1.5 mm
  for the buck's switch node, 2.0 mm with 0.3 mm clearance for the AUX 12 V path. Vias 0.6 / 0.3 mm (0.8 / 0.4 on the
  wide nets). These are standard 4-layer rules at JLCPCB / PCBWay.
- **Placement:**
  - Bays at the top left, with their envelopes kept clear of parts and the AUX strip above them along the top edge.
  - The sensor-pack core at the top right, with J3 and J10 on the right edge.
  - The screen transceivers, video decoder, power supply and harness connectors in the lower band.
  - Cable ports on the top edge. J7, J9, J6 and the trunk port on the bottom edge.

## Status

**Routed except two links**, which need a few minutes by hand in KiCad's interactive router (press **X** on the pad):

| Net | From | To | Why it is left |
| --- | --- | --- | --- |
| V_D6 | U8 pin 12 (front) | J1 pin 32 | Pin 12 of the TVP5150 is boxed in by its neighbours' traces |
| TACH_SIG | U1 pin 35 (front) | R56 pin 2 (back) | Under the 2560 there is no room left for its via |

DRC (`generate/drc.py`, with the schematic parity check): no errors apart from those two unconnected links. The rest
is silkscreen warnings (reference text at the edge and over pads) and eight parity notes for the bay headers'
MP1 / MP2: the M3 holes are tied to GND on the board, and the 2×5 header symbol has no pins for them. About 2070 track
segments and 1450 vias (1100+ of them GND stitching). 111 parts on the front, 118 on the back.

## Provisional (move with the bezel)

- **The screen headers J5 / J1 / J4 are exactly where rev 1 has them.** The bezel is not drawn yet. When it is, move them
  (and the four M3 mounting holes H1–H4), then re-run placement and routing.
- The board outline is rev 1's. Once the screen positions are known, the height can change.

## Check before ordering

- The TVP5150AM1 circuit against TI SLES209E (figure 6-1) and the TPS54560B values against its datasheet (figure 32).
  Both were copied from TI's typical applications.
- The power stage by eye: the input loop (C2–C5, U2, D6) should be tight, and the +5 V plane is fed through the output
  capacitors' vias. The 12 V and AUX paths' widths are set by net class; check the copper around Q2, Q5 and Q6 for
  the heat at full load.
- The parts on the back against the space behind the dash. The tallest are the 10 mm electrolytics, the fuse holder
  and the vertical connectors.
- Mechanical: the bays' envelopes against the carriers, and the standoff height.

## Firmware that has to follow (not done yet)

Port `arduino/sketches/sensor_pack` to this pin map (see the table in the plan): USART1 fan-out, ICP4 / ICP5,
FRAM on D20 / D21, PWR_HOLD, and the MCP2515 sending `0x100`–`0x104` and heartbeat `0x704` as node 0x04. The screens
take sensor frames from CAN with the UART as a fallback. The center stops re-sending when it hears `0x100` from
another node. `can_bus.md` and `sensor_uart.md` need the matching updates.

## Regenerating

KiCad 10, numpy in KiCad's python (`$KIPY -m pip install --user numpy`), Freerouting **1.9.0** on Java 17+:

```bash
KIPY=/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3
python3 generate/gen_footprints.py && python3 generate/gen_sch.py   # also writes generate/refs.json
JAVA=/opt/homebrew/opt/openjdk@17/bin/java $KIPY generate/gen_pcb.py --route freerouting-1.9.0.jar
python3 generate/drc.py
```

`gen_pcb.py` places everything, then (with `--route`):
1. gives each SMD GND / +5 V pad a via to its plane
2. runs Freerouting once
3. joins any leftover piece of a net
4. stitches the ground pours
5. fills the zones

`--from-ses` repeats steps 3–5 from the last Freerouting result. `--finish`, `--stitch`, `--fill` and `--labels` redo only
those steps. The finisher joins leftovers with straight, L-shaped, 45° and via paths, then a grid maze router.
`DIGIDASH_RIPUP=1` also lets it rip up nearby segments to get a stuck link through. That is off by default because at
this density it cascades. Freerouting's result changes with any placement change, so the leftovers differ from run
to run.
