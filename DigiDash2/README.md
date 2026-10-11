# DigiDash main board, rev 2

The dash's main board, redrawn per [`docs/hardware/digidash_rev2_plan.md`](../docs/hardware/digidash_rev2_plan.md):

- all SMD except connectors, every SMD part on the back (single-sided assembly); only the three screen headers are on
  the front
- the sensor pack is **a slot card** ([`SensorCard/`](../SensorCard)), not part of this board: it talks to the screens on
  CAN, and its serial line can reach them through **J27** on a jumper wire (the old UART fan-out, kept as a fallback)
- the TVP5150AM1 video decoder on the board
- six card slots (2×15 sockets; cards stand on edge) with AUX 12 V on the slot, an I2S rail bused to all six and an
  analog audio bus
- four cable ports and a trunk port

Rev 1 stays in [`DigiDash/`](../DigiDash) as the reference.

| Files | |
| --- | --- |
| `DigiDash2.kicad_sch` / `.kicad_pcb` / `.kicad_pro` | The board: 309.2 × 103.7 mm (rev 1's outline), 4 layers. KiCad 10 |
| `DigiDash2.pretty` | The RCA jack footprint (video input). Everything else is stock KiCad or `DigiDash_Accessory` |
| `generate/` | `gen_footprints.py`, `gen_sch.py`, `gen_pcb.py`, `drc.py` (see [Regenerating](#regenerating)) |
| `fab/DigiDash2_BOM_PCBWay.csv` | The assembler's BOM: the 154 placed parts with maker and part number (`$KIPY generate/gen_bom.py`) |

## What is on it

| Block | Parts | Notes |
| --- | --- | --- |
| Power in | J7 (Micro-Fit 3.0, 6 pin: 1–2 battery, 3–4 GND, 5 ignition, 6 illumination), Q2 reverse-polarity P-FET, SMCJ33A TVS | J7 carries about 10 A (two pins each for battery and ground) |
| 5 V | U2 TPS54560B, 5 A, 400 kHz: TI's typical application (datasheet figure 32) component for component | EN comes from the ignition through 442k / 90.9k (starts above about 6.5 V) with a 5.1 V clamp. A card holding the slots' WAKE line high also keeps it on: the sensor card does, after key-off, to finish its shutdown work |
| 3.3 V, 1.8 V | U11 AP7361C-33 (1 A), U12 AP2112K-1.8 | The board's own 3.3 V now feeds the CAN transceivers' VIO, the TVP5150's I/O, GPS and the I2C pull-ups (rev 1 borrowed the center screen's 3V3) |
| Switched 12 V | Q5 P-FET high-side switch, turned on by the ignition through Q4 | Feeds pin 8 of every slot, the trunk port and the camera |
| AUX 12 V | J13 (Mini-Fit Jr, vertical), 10 A mini-blade fuse, Q6 reverse P-FET, TVS | Slot power on pins 25–30 of every slot (no more bay plugs). Its return, AUX_GND, meets GND only at net tie NT1 |
| Sensor serial in | J27 (1×2 pin header, back side, top edge above slot 4) and one 10k / 20k divider per screen | Optional: a jumper wire from the sensor card's J8 brings its 5 V sensor-frame serial line here, and the dividers take it to 3.3 V on pin 15 of J5 / J1 / J4 (the screens' sensor UART RX). With no wire fitted the screens' RX sits low and the data comes from CAN |
| Screens | J5 / J1 / J4 (same pin use as rev 1), U5 / U9 / U10 MCP2562 | 120 Ω at the left end, and at the right end through SW1-1 (switch it off when the trunk port carries the bus on) |
| Video | U8 TVP5150AM1 (address 0x5C), 14.31818 MHz crystal, J11 RCA input (37.4 Ω + 37.4 Ω termination, per TI), anti-alias filter footprints (not fitted) | Same J1 pins as rev 1's module. RESETB: 10k / 2.2 µF power-on delay (about 26 ms), plus the center's GPIO52 (J1 pin 36) through 1k; the firmware pulses it at start-up. Reviewed against TI SLES209E on 2026-10-02: see [Video review](#video-review) |
| Card slots | J31–J36 (slot 1–6), 2×15 sockets, 25.4 mm apart (cards' parts up to 20 mm tall). Per slot: 0.5 A PTCs on 5 V and switched 12 V, 0.1 A on 3.3 V, 0.2 A on constant 12 V, a slot-ID resistor. Shared: CAN, WAKE (D-OR into the buck's EN), LAMP / REV levels (100k / 27k, clamped), the audio bus (its ground joins GND at NT2), AUX 12 V, and the I2S rail (BCK, WS, MIC, RET, MCLK on pins 15–18 and 24, bused to all six slots; the audio card is its clock master) | See [`accessory_ports.md`](../docs/hardware/accessory_ports.md#rev-2-digidash2-card-slots) |
| Cable ports, trunk | Cable ports J17, J22, J23, J24. Trunk J25 | As before |

## Connectors (what each one is, and what the silkscreen says)

Every connector carries its name and pin labels on the silkscreen, on the side you plug into. Where the pin labels
cannot fit beside the pins, a legend line ("1 FUEL  2 ETEMP ...") stands in for them. Pin 1 is the square pad.

| Ref | Silk name | What it is | Pins (1, 2, ...) |
| --- | --- | --- | --- |
| J5 / J1 / J4 | LEFT / CENTER / RIGHT SCREEN | The three round screens (front, face down). Their labels are on the back too | as rev 1 |
| J7 | POWER IN | Vehicle power, Micro-Fit 3.0, 6 pin | 12V, 12V, GND, GND, IGN (key on), LAMP (dash lights) |
| J13 | AUX 12V IN | Separate high-current 12 V for the slots (own fuse at the fuse box) | 12V, GND |
| J9 | CAMERA | Backup camera and reverse trigger, JST PH 4 pin | REV (reverse lamp 12 V in), 12V (switched camera power), unused, GND |
| J11 | VIDEO IN | Camera's composite video, RCA | tip = video, sleeve = GND |
| J6 | SERIAL | Serial port: the center screen's UART2 (GPIO3 TX, GPIO2 RX), 3.3 V logic, JST PH 4 pin. Was "GPS"; same pins | 3V3, TX (out of the dash: to the device's RX), RX (into the dash: from the device's TX), GND |
| J17, J22, J23, J24 | CABLE 1 .. 4 | Cable ports for off-board CAN devices (console, nodes near a sensor) | 5V, H (CANH), L (CANL), G |
| J25 | TRUNK | Bus extension to the rear of the car, Micro-Fit 3.0, on the bottom edge clear of the cards | H, L, 12V (switched), G. Switch SW1-1 off and terminate at the far end |
| J31 … J36 | SLOT 1 … 6 | Card slots, 2×15 female socket (square pad = pin 1, at the bottom). Slot 1 is the one for a card with tall parts | see `docs/hardware/accessory_ports.md` |
| **J16** | CAN TAP | **A tap on the CAN bus** for a USB-CAN adapter, to watch the traffic while debugging | H (CANH), L (CANL), GND |
| **J27** | SENSOR SERIAL | **Optional** sensor-frame serial in: a jumper wire (2-pin Dupont) from the sensor card's J8 (pin 2 TX, pin 1 GND). On the back at the top edge, above the slot row, within reach of a card in any slot | 1 TX IN (5 V logic, from the card), 2 GND |
| SW1 | 1 TERM  2 -- | Back-side 2-way slide switch. 1: the bus's right-end 120 ohm, on at the factory; off if a trunk cable carries the bus on. 2: spare (demo mode moved to the sensor card's J9 jumper) | |

Notes on the labels: J6's "TX" and "RX" are the dash's side: TX goes to the device's RX. The labels are added by
`gen_pcb.py` (`--labels` re-adds them to the current board) using free space beside each connector. The vehicle's
sensor wiring (senders, speed, tach) now plugs into the sensor card, not this board: see
[`SensorCard/README.md`](../SensorCard/README.md).

## Board

- **Layers:** F signal, In1 solid GND, In2 solid +5 V, B signal. Every SMD GND and +5 V pad has its own via to its plane.
  The F and B ground pours are stitched to In1.
- **Sides:** every SMD part is on the back, so the board is assembled from one side. Only the three screen headers
  (J1, J4, J5) are on the front.
- **Rules:** 0.2 mm tracks and 0.13 mm clearance by default. 0.4 mm for 3.3 / 1.8 V, 0.8 mm for the 12 V nets, 1.5 mm
  for the buck's switch node, 2.0 mm with 0.3 mm clearance for the AUX 12 V path, 0.25 mm with 0.2 mm clearance for the video input. Vias 0.6 / 0.3 mm (0.8 / 0.4 on the
  wide nets). These are standard 4-layer rules at JLCPCB / PCBWay.
- **Placement:**
  - The six slots in a row at the left (sockets centred at y 53, x 20 … 147, 25.4 mm apart), the AUX strip above them along the
    top edge. Under each card's envelope (y 13 … 93) only parts under about 7 mm: the tall ones (electrolytics, fuse
    holder, inductor) are kept out, and the placer prints a "check:" line for any through-hole part it finds there.
  - J27 (sensor serial in) above slot 4 on the top edge, outside the card envelopes. The top-right area the sensor
    pack used to take is empty.
  - The screen transceivers, video decoder, power supply and harness connectors in the lower band.
  - Cable ports on the top edge. J7, J9, J6 and the trunk port on the bottom edge.

## Status

**Placed and fully routed, single-sided assembly** (2026-10-05). Every SMD part is on the back; only the three screen
headers (J1, J4, J5) are on the front. One Freerouting run (120 passes) left one `+1V8` link open at U8; the finisher
with rip-up (`DIGIDASH_RIPUP=1 $KIPY generate/gen_pcb.py --finish`) closed it. Then a 3 mm dead-end stub on `CL_SDA`
(left by the rip-up, near x 149, y 85) was trimmed by hand. DRC (`generate/drc.py`, with the schematic parity
check): **no errors, nothing unconnected, parity clean.** What is left: 56 silkscreen warnings (reference text at the
edge, over pads, overlaps), and one 7 µm dangling sliver where the pre-drawn `VID_AIP1A` route meets the routed track
(physically nothing; trimming it exposes the pre-drawn route, so it stays). 1872 track segments and 1491 vias (1352 of
them GND). 3 parts on the front, 181 on the back. Fab outputs are in `fab/` (`python3 ../tools/fab_export.py
DigiDash2`); the not-fitted parts (C42, C43) and the through-hole parts are left out of the placement files. (Run in a
container without KiCad's global library table, DRC also lists 177 "library not in the configuration" notes; they
are about the environment, not the board.)

The two-sided version (69 parts on the front), routed and DRC-clean, is at commit `af4628e`.

How it got there, in case a re-run differs:
- **Sensor pack removed, slots widened.** The first route after the sensor pack left (slots still at 20.32 mm) left
  five links open around the video decoder. With the pitch at 25.4 mm, U11 (3.3 V LDO), J11 (RCA), J16 and D2 moved
  east of slot 6, and the next run routed completely.
- **Video review (2026-10-02).** The video support parts are fixed by description (`FIXED_DESCR`), and the input
  (`VID_ATT`, `VID_AIP1A`) and the pixel clock's first 2 mm are drawn and locked before routing (`PREROUTE`). Left to
  itself, Freerouting took `VID_ATT` 80–150 mm round J1. The post-route checks now keep each net class's own clearance.
  YOUT4 / YOUT5 also leave the pin row as a pre-drawn pair, and the RESETB pull-up (R39) is fixed on the back. In one
  run the two data lines boxed each other in, with R39 on the front right under the pins.
- **Single-sided (2026-10-05).** Every SMD part moved to the back. U8 (TVP5150) is mirrored there (analog side east, data
  pins toward J1), and the video input chain sits between J1's east end and the RCA jack. The center screen's
  sensor-serial divider is fixed under J1 pin 15: left to the placer it was walled in between slots 4 and 5.
- **U8 (TVP5150) is at x 132,** on the back between slot 5's and slot 6's socket pins. A fixed part's spot is not
  checked against the other side's through-hole pins, so keep that in mind when moving anything near the slots.

## Provisional (move with the bezel)

- **The screen headers J5 / J1 / J4 are exactly where rev 1 has them.** The bezel is not drawn yet. When it is, move them
  (and the four M3 mounting holes H1–H4), then re-run placement and routing.
- The board outline is rev 1's. Once the screen positions are known, the height can change.

## Video review

The TVP5150AM1 circuit was checked line by line against TI SLES209E (2026-10-02):

- **Matches the datasheet:** the pinout; the I2C address strap (YOUT7 10k down: 0xB8 / 7-bit 0x5C); the INTREQ 10k pull-down that TI
  requires when the pin is disabled; the 14.31818 MHz crystal and 100k across it (load caps 27 pF since 2026-10-05, for the Abracon ABM8's 18 pF CL); the 1 µF REFP / REFM / REFP–REFM caps;
  the 37.4 Ω + 37.4 Ω termination with 0.1 µF coupling (and AIP1B to ground through 0.1 µF); PDN held high; the supplies
  (3.3 V I/O, 1.8 V core through a ferrite for the analog side) with a 0.1 µF at every supply pin. The center firmware's
  register writes (composite on AIP1A, auto standard, BT.656 8-bit with embedded syncs, outputs on) and its chip-ID check
  (0x51 / 0x50 at 0x80 / 0x81) fit the part.
- **Changed in this review:**
  - The ESD clamp and the termination now sit at the RCA jack (the clamp had been placed across the board, which made the
    unterminated input about 178 mm long).
  - The crystal and its load caps sit on the back, right under XTAL1 / XTAL2. The 22 Ω pixel-clock damper sits under SCLK.
  - The input nets get their own net class (`Video`, 0.25 mm track, 0.2 mm clearance) and a hand-drawn path. The jack to the termination is 5.8 mm; the terminated signal runs 66 mm to the coupling cap, mostly on B.Cu under J1, with 2 vias. The cap to AIP1A is 2.3 mm and SCLK to its damper is 3 mm.
  - RESETB: the 100k / 1 µF became 10k / 2.2 µF, with 1k from GPIO52. The old 100k could have been held under the
    0.7 × IO_DVDD threshold by a weak default pull-down on GPIO52, which would leave the decoder in reset.
- **Anti-aliasing filter: footprints only (DNP).** TI's note F calls it "highly recommended for best video quality". The
  filter sits between the ESD clamp and the termination: C42 (220 pF, VIDEO_IN to GND), R53, and C43 (220 pF, VID_AAF to GND).
  It is a 3rd-order Butterworth C-L-C at 75 Ω with a 9 MHz cutoff: about 0.1 dB down at 5 MHz and about 24 dB down at
  22.5 MHz, which would otherwise fold back into the picture at the 27 MHz sample rate. **As delivered:** R53 is a 0 Ω
  (0805) and C42 / C43 are empty, so the circuit is TI's figure 6-1 without the filter. **To fit it:** replace R53 with a
  2.7 µH 0805 inductor (self-resonance well above 50 MHz) and fit C42 and C43 with 220 pF C0G 0603.
- **Not checked here:** the center module's header-pin-to-GPIO map (data on GPIO20–22 / 28–32, PCLK 49, VSYNC 50, RESETB
  52, cluster I2C 47 / 48). It is rev 1's wiring for the plug-in module; check it against Waveshare's
  ESP32-P4 pinout once more before ordering.

## Check before ordering

- The TPS54560B values against its datasheet (figure 32), copied from TI's typical application.
- The power stage by eye: the input loop (C2–C5, U2, D6) should be tight, and the +5 V plane is fed through the output
  capacitors' vias. The 12 V and AUX paths' widths are set by net class; check the copper around Q2, Q5 and Q6 for
  the heat at full load.
- The parts on the back against the space behind the dash. The tallest are the 10 mm electrolytics, the fuse holder
  and the vertical connectors.
- Mechanical: the slot pitch (25.4 mm) and the cards' 80 × 60 mm envelope against the case, and the height of every
  main-board part under a card (under about 7 mm; the card's bottom edge sits about 8.5 mm up).
- The slot socket footprint is our own (`DigiDash2.pretty/Slot_Socket_2x15_P2.54mm_Vertical`, numbered to mate with the
  card's right-angle header). Check pin 1 against a card in the 3D viewer before ordering.

## Firmware that has to follow (not done yet)

The sensor pack's firmware now targets the sensor card: see [`SensorCard/README.md`](../SensorCard/README.md) for its
pin map. The screens take sensor frames from CAN (node 0x04, `0x100`–`0x104`) with the UART (through J27, when the
jumper wire is fitted) as a fallback. The center stops re-sending when it hears `0x100` from another node.
`can_bus.md` and `sensor_uart.md` need the matching updates.

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
