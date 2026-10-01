# DigiDash rev 2: re-layout plan

**Status: built in `DigiDash2/` (routed except two links, see its README). Firmware steps not done yet.** Rev 1 is the board in `DigiDash/` today (2 layers, 309 × 104 mm, every
part through-hole, with plug-in dev boards for the FRAM and the video decoder). Rev 2 is made to be fabbed and
assembled by a board house, so:
- It goes SMD for everything that isn't a connector.
- It puts the FRAM and the TVP5150 video decoder directly on the board.
- It replaces the ATmega328P with an **ATmega2560** on CAN.
- It trades the four scattered accessory ports for **six card slots** (first drawn as two double bays) plus more cable
  ports.

> **Update 2026-10-01: bays → card slots.** The flat carriers proved too small, so the two bays are replaced by **six
> 2×15 slots** for cards that stand on edge (80 × 60 mm: 80 along the main board, 60 tall), with their connectors on their
> top edge. The slot adds a 3.3 V and a constant 12 V feed, a slot ID, a WAKE line, the dash-light and reverse levels,
> five link lines between neighbour-pair slots (1–2, 3–4, 5–6), a shared analog audio bus and AUX 12 V on the slot
> itself (the bay power plugs J14 / J15 are gone). J6 is now a plain **serial port** (the center's UART2) instead of
> "GPS": GPS moves to a card. The spec is [`accessory_ports.md`](accessory_ports.md#rev-2-digidash2-card-slots); the
> card ideas are under [Expansion cards](#expansion-cards). Sections below that talk about bays are the earlier plan,
> kept for the reasoning.

## Goals

1. **Bays, not leftover spots.** Every accessory position has the same shape, the same header location inside it and
   the same keep-outs, so a carrier drawn once fits any bay.
2. **Two double bays.** A bay is two of today's positions stacked: **50 × 52 mm**, with a header at A and at B. A
   single-size carrier (50 × 26, like the current audio carrier) uses one half; a double carrier uses both and can
   carry panel parts such as the four RCA jacks and the 3.5 mm jack. Two bays give up to 4 single carriers, 2 double,
   or 1 double + 2 single. **Four cable ports** (up from two) for accessories that belong elsewhere in the car. See
   [How many bays](#how-many-bays-use-cases).
3. **No dev boards.** SMD everywhere except connectors. The FRAM and the TVP5150 are on the board, and the resistor pack
   becomes discrete resistors. This frees most of the back and shrinks the "core" (MCU, power, CAN, video) to one area.
4. **ATmega2560** for the sensor pack: spare pins, four UARTs, 16 ADC channels, input capture for speed and tach.
5. **Sensor pack on CAN, serial as fallback.** The sensor pack sends its own frames on the bus through a new
   **MCP2515 + MCP2562**, so no node depends on the center to re-broadcast them. The serial fan-out stays as a fallback
   for this revision.
6. Keep what works: the screen headers and their straps, the CAN bus layout (terminated at the left and right screens),
   the port pinout and hole pattern, the harness connectors.

## What rev 1 does badly (from the current board and schematic)

| Area | Rev 1 | Consequence |
| --- | --- | --- |
| Accessory ports J18–J21 | Four 2×5 positions at (−74, 28.5), (−16, 28.5), (−84, 74), (−84.7, 97.3), each rotated differently | Every carrier needs its own check against neighbours; J20 / J21 share an area with the fuses F5–F8 between them |
| Parts | All THT: DIP-28 MCU, 3 × DIP-8 transceivers, axial resistors, disc caps, TO-220 regulator, 12 mm inductor, 1000 µF can, 17 mm standing PTCs (TH1–TH4) | Parts spread over the whole back, and tall parts create keep-outs |
| ATmega328P (U1) | Every pin used; D11–D13 double as the ISP pins; PD0 (RX) unconnected | No room for new inputs, no return path for the center's 0xA5 keepalive, speed / tach timed with `micros()` |
| Sensor data path | 328P UART TX fanned out to all three screens through the RN1 divider; the center re-sends it on CAN | The sensor pack is not a CAN node, so the data depends on the center being up |
| FRAM | Separate FRAM board (FRAM1, 1×8 header) on A4/A5 | Another plug-in board for one SOIC-8 chip |
| J11 / J12 | RCA jacks on the left edge; the tip net only joins the two jacks and the sleeves are unconnected | Orphans from the old amplifier sockets: remove |
| Power | LM2576T-5 + 100 µH / 3 A inductor + 1N5822 + 1000 µF | About 35 × 25 mm of tall parts; 52 kHz switcher |

## Fixed inputs and what's still open

Because a board house builds it, the outline is **not** limited by fab or hand assembly. The limits are mechanical:
the width is set by the three screens, and the height and depth by the space behind the bezel. These have to be
settled first, because the whole layout hangs off them:

- **Screen positions against the bezel.** Rev 1's headers (J5 left at (−141.8, 108.5), J1 center at (−36.4, 91),
  J4 right at (103.5, 108.5)) were placed before the bezel was drawn, and the port spec already says they will move.
  Rev 2 needs the three screen centres and the bezel opening. The screen headers' pins also come through to the back, so
  their final spots set the lower edge of the bay band.
- **Depth behind the board.** Sets the height limit for double carriers. The audio carrier with RCJ-02 jacks is about
  22 mm plus the 11 mm stack.
- **Mounting holes** for the board itself, and where the harness connectors must exit (today: J6 GPS, J7 power, J9
  camera, J10 speed / tach along the bottom edge; J3 sensors and J2 ISP on the right edge).
- **Assembly.** SMD with a TQFP-100 is fine for JLCPCB / PCBWay assembly. By hand it is doable (drag-solder the TQFP) but
  slow. The plan assumes machine assembly of the SMD side, with the connectors soldered by hand.

## How many bays (use cases)

The accessory protocol (`can_channels.md`) defines node types for sensor nodes, input devices, audio sinks, and output /
relay nodes. Going through what each kind of accessory would actually do, the key question is **where it wants to
live**: on a bay behind the dash, or out in the car on a cable.

| Accessory | Node type | Best home | Size | Why |
| --- | --- | --- | --- | --- |
| **Bluetooth audio** (DACs, mic preamp, RCA + TRS jacks) | audio | **bay, double** | 50 × 52 | Nothing to wire out but line-level cables to the amp; the jacks want to be on the board |
| **Output / relay node** (MOSFET or relay drivers for lights, fans, accessory power) | output | bay, single or double | 26–52 | Switches loads from the dash's own ignition-switched 12 V; its harness leaves from the back of the dash |
| **GPS / IMU / RTC** (GPS on CAN instead of the center's UART, a tilt or g sensor, a clock that keeps time with the key off) | sensor | bay, single | 50 × 26 | Small, with only an antenna lead, and it has nowhere better to be |
| **Data logger / Wi-Fi or BLE bridge** | sensor | bay, single | 50 × 26 | Small. The center already has SD and Wi-Fi, so this is optional |
| **Extra gauges** (trans temp in the doc's example, wideband O2, boost, EGT) | sensor | **cable port** | any | Short sender wires beat a long analog run to the dash. The node sits near the sensor and sends numbers on CAN |
| **Console** (knobs + OLED, ESP32-S3) | input | cable port | off-board | Exists: J17 today |
| **Steering-wheel / remote buttons, IR** | input | cable port | off-board | Lives where the hands are |
| **Parking sensors, rear lighting, TPMS receiver** | sensor / output | cable port, at the rear | off-board | Needs to be at the back of the car. See the trunk note below |

**What that says:** the bays fill up with things that have no natural home elsewhere: audio, an output node, a GPS /
clock board. Most new *sensor* ideas are better as small nodes out in the car on a cable port. So:

- **Two double bays are enough.** Bay 1 is the audio carrier (double). Bay 2 is flexible: one double (an output node
  with room for relays or big MOSFETs) or two singles (for example GPS / RTC + a small output node). With two A / B
  headers per bay that is up to four carriers, the same count as rev 1's four ports but without the geometry fights.
- **Spend the saved space on cable ports.** Four JST PH 4-pin ports (5 V, CANH, CANL, GND, each 5 V feed on its own PTC)
  instead of two. They are cheap and cover the "node near the sensor" cases.
- **Trunk port for the rear of the car** (see [Trunk port](#trunk-port-bus-extension)).
- **Real 12 V power at the bays** (see [Bay power](#bay-power-aux-12-v)). An amplifier or an output node driving fans
  and lights needs amps, not the 0.5 A on the header's pin 8.

### Trunk port (bus extension)

CAN is a line terminated at both ends. Branches off it must stay under about 30 cm at 500 kbit/s. Today the line runs
across the board from the left screen (120 Ω) to the right screen (120 Ω), and the cable ports are short branches. A
node at the rear of the car (parking sensors, rear lights, a bed / tailgate relay, a TPMS receiver) is 4–5 m of cable
away. That is far too long for a branch: reflections would corrupt frames for every node, the screens included. So the
line itself is extended:

- **Connector:** locking Micro-Fit 3.0, 4-pin: CANH, CANL (twisted pair in the cable), **switched 12 V** (about 1 A,
  fused), GND. A remote node makes its own 5 V, because 5 V sent 5 m drops too much.
- **Termination moves.** The right screen's 120 Ω goes on a jumper, fitted by default ("terminated here"). With a trunk
  cable in use, it comes off, and the last node on the cable carries the 120 Ω. No firmware change: termination is
  purely electrical.
- **Protection at the port:** its own CAN ESD clamp (a second NUP2105L) and a footprint for a common-mode choke,
  because this cable runs through the car.
- On the board from the start; it can be left unpopulated until a rear node exists.

### Bay power (aux 12 V)

The 2×5 bay header stays as it is (5 V, switched 12 V on pin 8 at 0.5 A for logic and "ignition on", CAN), so every
carrier stays compatible. High-current 12 V gets a **separate path from input to bay**, so amplifier or load current
never flows through the logic board's ground under the screens, the sensor inputs or the video decoder:

- **Its own input:** "AUX 12V IN", a 2-pin Mini-Fit Jr (about 9 A per contact), wired to its own fuse at the car's fuse
  box (10–15 A). It is separate from J7, which powers the dash itself.
- **Protection at the board:** a reverse-polarity P-FET (or ideal-diode controller), a TVS clamp, and a board fuse or
  electronic fuse sized for the bays together (about 10 A).
- **Its own copper:** a wide 12 V run and a separate return along the board edge to the bays. The return joins the
  logic ground at **one point**, at the power entry.
- **A power connector per bay:** 4-pin Micro-Fit 3.0 (2 × 12 V, 2 × GND, about 10 A) at the bay's edge. A carrier that
  needs power plugs in with a short pigtail. A cable tolerates the stacking tolerances better than a second rigid
  board-to-board connector would. Carriers that don't need it ignore it.
- **Switched or constant?** Feed AUX from an ignition- or accessory-switched fuse. A carrier that must run with the key
  off (a clock, an alarm) uses pin 8 for "key on" and draws microamps.

This makes an amplifier carrier possible: for example 2 × 25 W class-D in the audio bay, where the current design is
line-out to an external amp. It also covers an output node switching real loads.

## Proposed arrangement (back side, looking at the back)

Rough zones on a 309 mm wide board (the three screens set the width). The height can change once the bezel and cavity
are known; two bays leave room to spare. Final numbers come after the screen and bezel positions are fixed.

```
 x: -149                                                                           +160
  +-----------------+-----------------+----------------------------------------------+  y 12
  |  BAY 1 (double) |  BAY 2 (double) |  CORE                        cable ports 1-4 |
  |  50 x 52        |  50 x 52        |  ATmega2560, FRAM, MCP2515 + MCP2562         |
  |  [A header]     |  [A header]     |  sensor input conditioning, RPM select       |
  |  [B header]     |  [B header]     |  J3 sensors, J2 ISP, FTDI header  (right edge)|
  |  audio          |  flexible       |                                              |
  +-----------------+-----------------+----------------------------------------------+  y ~66
  |  U5 + J5 hdr        POWER (buck, TVS, 3.3 / 1.8 V)   U9 + J1 hdr   TVP5150  U10 + J4 |
  |  (left screen)      J7 power in                      (center)      + RCA    (right)  |
  |  J6 GPS    J9 camera    J10 speed / tach      trunk port (bus extension)            |  y 116
  +--------------------------------------------------------------------------------------+
```

- **Bays (top left).** Two double bays side by side, 52 mm wide each. Each bay has **two** header positions, A and B,
  stacked 26 mm apart, both with the standard 2×5 pinout, the asymmetric M3 holes and their own 0.5 A PTCs. A single
  carrier plugs into A or B. A double carrier plugs into A and covers B, so its underside keeps clear of B's shrouded
  header (about 9 mm tall, inside the 11 mm stack; carriers keep a 21.5 × 10 mm pocket free of bottom-side parts and pins there).
- **Core (top middle and right).**
  - The 2560, its crystal and FRAM, and the sensor pack's MCP2515 + MCP2562 (a short stub to the CAN pair).
  - The sensor input conditioning (dividers, PTCs, clamps) and the RPM-select jumpers.
  - J3 (sensors), J2 (ISP) and the new FTDI header on the right edge.
  - Four cable ports along the top edge.
- **Lower band.**
  - Each screen's MCP2562 sits next to its header.
  - Power sits in the middle near J7.
  - The TVP5150 sits right next to the center header, with its composite input jack on the bottom edge by J9.
  - Harness connectors stay along the bottom edge, plus the trunk port.
  - The CAN pair runs once along this band from U5 (120 Ω) to U10 (120 Ω, switchable). The bay headers and cable
    ports tap it as short stubs.
- **Front** stays the three screens only.

The single-size audio carrier I just routed fits half a bay as-is. A double-bay version could put the four RCJ-02
jacks and the SJ1-3523N jack back on the board, and likely go back to 2 layers.

## ATmega2560 (sensor pack)

ATmega2560-16AU, TQFP-100, 5 V, 16 MHz crystal: the same clock and voltage as today, so `sensor_pack.ino` ports with
pin-map changes only. The sketch already notes I2C moves to D20/D21 on a Mega (sensor_pack.ino:39).

| Function | 328P today | 2560 plan |
| --- | --- | --- |
| Sensor frames (primary) | none: the center re-sends the UART frames on CAN | **CAN**, `0x100`–`0x104` sent by the sensor pack itself (node `0x04`), through an MCP2515 on SPI (SS, SCK, MOSI, MISO, plus INT on an external interrupt) and an MCP2562 |
| Sensor frames (fallback) | TX0 (D1) → RN1 divider → pin 15 of J1 / J4 / J5 | The same fan-out and frame format on **USART1**, sent at the same time, so a screen can fall back to it |
| Keepalive from the center | never arrives (PD0 unconnected) | Heard on CAN (the center's `0x213` status or `0x701` heartbeat); **RX1** from the center is wired as well, for the fallback path |
| Debug / USB-serial | shares D0 / D1 | **USART0** alone on a 6-pin FTDI header (programming and log output without unplugging the screens) |
| Speed (hall) | INT0 + `micros()` | **ICP4 / ICP5** input capture: hardware timestamps, no interrupt jitter |
| Tach (coil) | INT1 + `micros()` | the other input-capture pin |
| Fuel / temp / oil / ambient | ADC0–3 | ADC0–3, plus spare channels for battery voltage, a second fuel sender, etc. |
| Fuse monitors, ignition, alt L | D4–D10 | any port; no more sharing with the ISP pins |
| RPM select | D5 / D12 (D12 shared with MISO) | two dedicated pins |
| Demo mode | D11 (shared with MOSI) | a dedicated pin with its pull-down |
| FRAM | FRAM board on A4 / A5 | MB85RC256V (SOIC-8) on the board, TWI on D20 / D21 |
| ISP | J2, shares D11–D13 | J2 on the 2560's own SPI pins. The MCP2515 shares that bus on its own chip-select, pulled high so the chip stays off the bus while the 2560 is being programmed |
| Spare | none | about 60 I/O, 12 ADC channels, 2 USARTs |

### Sensor pack on CAN (decided: CAN primary, serial fallback)

The 2560 has no CAN controller, so rev 2 **adds two chips** to the core:

| Part | Package | Job |
| --- | --- | --- |
| **MCP2515** (-I/SO) | SOIC-18 | CAN controller on the 2560's SPI bus: its own chip-select, INT to an external interrupt pin, RESET tied to the 2560's reset (or a spare pin). Clock: **its own 16 MHz crystal** (3225 SMD, load caps per the crystal, typically 18–22 pF), not the 2560's CLKO: that pin only runs with the CKOUT fuse programmed, which standard Mega fuse settings leave off, so a re-flashed chip could silently take the sensor pack off the bus. 16 MHz is also the common MCP2515 library default. 100 nF decoupling |
| **MCP2562** (-E/SN) | SOIC-8 | Transceiver, wired like the screens' three: VDD 5 V, VIO 5 V (the 2560 runs at 5 V), STBY to GND, 100 nF on each supply. **No 120 Ω**: the bus stays terminated only at the left and right screens |

**How it runs:**
- **Node `0x04`** (free since the console moved to `0x13`) with the usual 1 Hz heartbeat `0x704`, node type 3
  (sensor pack).
- **Same frames as today.** It sends `0x100`–`0x104` at 20 Hz with exactly the IDs and payloads the center sends now,
  so the screens and accessories need no parsing changes. The odometer stays owned by the sensor pack (FRAM), as today.
- **The center backs off.** Two nodes must not send the same ID with different data, so the center re-sends
  `0x100`–`0x104` from its UART **only if it has not heard `0x100` from another node for 200 ms**. The same center
  firmware then works on rev 1 (sensor pack on serial only) and rev 2 (sensor pack on CAN). The center keeps producing
  the converted SENSOR channel for accessories (`can_channels.md`) from whichever source it has.
- **Serial fallback.** The sensor pack also keeps sending the 36-byte UART frame on USART1 to all three screens. Each
  screen uses the CAN frames and switches to the UART frames if no `0x100` has arrived for 400 ms (the same timeout as
  the NO DATA dim), switching back once CAN frames resume. A bus fault (shorted pair, babbling node) then costs the
  accessories their data but not the gauges. This softens the rule in `can_bus.md` that "the gauges never depend on
  CAN": they prefer CAN but do not need it.
- **Keepalive.** The sensor pack treats the center's status frame `0x213` or heartbeat `0x701` as the keepalive, and
  still accepts the 0xA5 byte on RX1.
- **Later:** once the CAN path has been proven in the car, rev 3 can drop the UART fan-out and its divider.

An AVR with a built-in CAN controller (AT90CAN128, TQFP-64) would save the MCP2515, but it has a smaller pin budget and
is a less common part. The 2560 plus MCP2515 is the easier path.

**Programming note:** the SparkFun Pocket AVR Programmer / USBtinyISP is known to be unreliable above 128 KB of flash,
which the 2560 has (256 KB). A USBasp, an "Arduino as ISP", or an Atmel-ICE / AVRISP mkII is the safe choice. Once a
bootloader is on it, the USART0 header takes uploads like a Mega.

## Video decoder on the board (TVP5150AM1)

Rev 1 plugs a TVP5150 module into J13 and wires its outputs to the center screen's camera pins. Rev 2 puts the chip on
the board and keeps **the same signals on the same center header pins**, so the center's camera code does not change.

| Item | Plan (check every value against the TVP5150AM1 datasheet, SLES209, at schematic time) |
| --- | --- |
| Chip | TVP5150AM1, 32-pin TQFP, right beside J1 (the center header), so the 8-bit bus + PCLK run is a few cm |
| Outputs | D0–D7, PCLK, HS, VS, FID to the same J1 pins as today (nets `V_D0`–`V_D7`, `V_PCLK`, `V_HS`, `V_VS`, `V_FID`). Series 22–33 Ω on PCLK (and optionally the data lines) at the chip, for edge ringing |
| Control | I2C on the cluster bus (`CL_SDA` / `CL_SCL`, GPIO47 / 48), address 0x5C / 0x5D by its I2C-select strap, the same as the module. Reset: RC on the reset pin so it comes up on its own, plus the center's GPIO52 (J1 pin 36, the `V_P17` net the module used) so the center can reset it |
| Clock | 14.31818 MHz crystal + load caps |
| Supplies | 1.8 V analog / digital / PLL and 3.3 V I/O (matching the P4's 3.3 V camera pins). Rev 1 feeds the module from the center screen's 3V3 pin; rev 2 gets its **own 3.3 V LDO** (which also feeds J6 GPS) and a **1.8 V LDO** from the 5 V rail, so the video no longer loads the screen's regulator. Ferrite + separate decoupling on the analog supply |
| Input | Composite video: a board-edge **RCA jack** (right-angle, THT) near J9, since camera cables end in RCA plugs. Then 75 Ω termination to ground, AC coupling (0.1 µF), and ESD protection at the jack. J9 keeps its reverse trigger (pin 1) and camera 12 V (pin 2); its spare pin 3 stays free |
| Layout | Analog input and its ground kept away from the 2560, the buck and the CAN pair; the digital bus kept short and over solid ground |

## SMD conversion (rev 1 part → rev 2)

| Rev 1 | Rev 2 |
| --- | --- |
| ATmega328P-P (DIP-28) | ATmega2560-16AU (TQFP-100) |
| 16 MHz HC-49 crystal + 22 pF discs | 16 MHz 3225 SMD crystal + 0603 caps |
| MCP2562-E/P (DIP-8) ×3 | MCP2562-E/SN (SOIC-8) ×3 for the screens, **+1 for the sensor pack** |
| (none) | **MCP2515-I/SO (SOIC-18)**, the sensor pack's CAN controller, new in rev 2, with its own 16 MHz 3225 crystal and load caps |
| PC817 (DIP-4) | PC817 SMD / EL357N (SOP-4) |
| 2N3904 (TO-92) | MMBT3904 (SOT-23) |
| Axial resistors, disc caps | 0603 / 0805 |
| RN1 (DIP-16 resistor pack: the level-shift dividers on the sensor TX fan-out) | **Individual 0603 resistors**, one divider per screen feed. They are easier to place, to change and to source |
| MF-R050 radial PTCs F1–F10 (+ new bays) | Bourns MF-MSMF050 / 1812 SMD PTCs |
| TH1–TH4 standing PTCs (sensor feeds) | 1206 / 1812 SMD PTCs, same hold current |
| FRAM board (FRAM1) | **MB85RC256V, SOIC-8, on the board** next to the 2560 (TWI, address 0x50 as today, WP and the address pins to GND, 100 nF) |
| LM2576T-5, 100 µH, 1N5822, 1000 µF | Synchronous buck (for example TI LMR33630 or LM76003 class, 3–5 A), shielded SMD inductor, ceramics plus one low-profile electrolytic, SMCJ TVS, reverse-polarity P-FET, input LC filter |
| NUP2105L | unchanged (already SMD) |
| J11 / J12 RCA | removed (orphans) |
| TVP5150 module on J13 | **TVP5150AM1 on the board** (see [Video decoder](#video-decoder-on-the-board-tvp5150am1)), with a 14.31818 MHz crystal, 3.3 V and 1.8 V LDOs, and an RCA input jack |
| 3.3 V from the center screen's pin 18 (GPS, video module) | Its own 3.3 V LDO on the board for GPS and the TVP5150's I/O. The cluster-bus pull-ups can stay on the center's 3V3 as today |
| J16 CAN debug header | keep (THT) |
| J17 / J22 cable ports | **four** cable ports (JST PH 4-pin, as today), plus one **trunk port** (locking Micro-Fit 3.0, 4-pin); the right screen's 120 Ω on a jumper |
| J7 vehicle power (JST PH 4-pin, about 2 A per contact) | **Upgraded to a locking Micro-Fit 3.0** (about 5 A per contact), same pin functions. Rev 2's input current (screens + accessories through the buck, the 12 V feeds, the trunk port) is more than PH contacts carry |
| (none) | **AUX 12V IN** (2-pin Mini-Fit Jr) and a **4-pin Micro-Fit 3.0 power connector at each bay** (see [Bay power](#bay-power-aux-12-v)) |
| Other harness connectors (J3 sensors, J6 GPS, J9 camera, J10 speed / tach) | stay JST PH: signal-level currents only. Check each against its real current when the schematic is drawn, and upgrade any that are close |
| Screen 2×20 headers, 2×5 port headers, JST PH harness connectors, J2 ISP | stay through-hole (mechanical strength) |

**5 V budget.** The loads are three ESP32-P4 screens, up to four bay positions and four cable ports at 0.5 A each, the
video decoder and the core. Everything at once is more than the LM2576's 3 A. Size the new buck for the screens plus
a realistic accessory load (about 4–5 A). The PTCs still limit each position.

**Layers.** Start with 2 layers. With SMD, the core and the CAN pair are easy. If the 2560's fan-out plus the video
module gets tight, 4 layers at 309 × 104 mm is the fallback; price it before deciding.

## Work plan

1. **Lock the mechanics:** screen centres and bezel opening, board mounting holes, depth behind the board, harness
   exits. (You)
2. **Update the port spec** (`accessory_ports.md`): two bays with A / B positions, the double-carrier envelope, the
   underside pocket over B's header, the height limit, four cable ports, the trunk port with its movable termination,
   and the bay power connector (pinout, current limit, pigtail).
3. **Schematic:**
   - 2560 core, FRAM (MB85RC256V) on board, sensor front-end in SMD, discrete dividers in place of RN1.
   - TVP5150AM1 with its crystal, 3.3 V and 1.8 V LDOs, RCA input and reset, on the same J1 pins as the module.
   - The sensor pack's **MCP2515 + MCP2562** on the 2560's SPI bus, joined to the CAN pair.
   - Keep the UART fan-out (USART1 TX through a divider to pin 15 of the three screen headers) and add RX1 from the
     center.
   - New buck supply; two bays (four header positions), four cable ports and the trunk port, each with its PTCs.
   - J7 on Micro-Fit 3.0. The AUX 12V input with its protection, the separate high-current path and return, and a power
     connector at each bay.
   - Remove J11 / J12, the FRAM board header, J13 and RN1.
   - Keep net names where the function is unchanged, so firmware docs stay valid.
4. **Firmware (parallel):**
   - Port `sensor_pack.ino` to the 2560 pin map: input capture for speed / tach, USART1 fan-out, RX keepalive.
   - Add the MCP2515 driver, sending `0x100`–`0x104` and the `0x704` heartbeat.
   - Screens (`esp32_p4_common`): take sensor frames from CAN, with the UART fallback on a 400 ms timeout.
   - Center: stop re-sending when another node sends `0x100`.
   - Update `can_bus.md` (node `0x04`, the back-off rule, the softened "never depend on CAN" rule) and `sensor_uart.md`
     (now the fallback path).
5. **Placement:** fixed parts first (screen headers, harness connectors, bay headers on their grid), then the core,
   power and transceivers.
6. **Routing and DRC:** the same generator-plus-Freerouting approach as the audio carrier, then a manual review of the
   power stage and the CAN pair.
7. **Double-bay audio carrier:** redraw it at 50 × 52 with the RCA and TRS jacks on board.
8. **Order, then bench bring-up** (bench testing only, per the plan for the car).

## Decisions needed

1. Screen / bezel geometry and depth behind the board. This is a long-lead item that blocks only placement (step 5)
   and sets the board height. The port spec, schematic and firmware (steps 2–4) go ahead without it.

Decided:
- The board is fabbed and assembled by a board house. The outline is set by the screens and the dash, not by fab.
- **Six card slots** (2×15, cards on edge, links between neighbour pairs) and four cable ports. This replaced the
  earlier decision of two double bays (four header positions).
- CAN stays the only shared data bus. Real-time streams between two cards (I2S audio) use the links; bulk network data
  uses Wi-Fi (see [Expansion cards](#expansion-cards)).
- The sensor pack is on CAN (MCP2515 + MCP2562, node `0x04`), with the serial fan-out kept as a fallback.
- The MCP2515 has its own 16 MHz crystal. It does not use the 2560's CLKO.
- No dev boards: TVP5150AM1 and MB85RC256V on the board, and RN1 replaced by individual resistors.
- Undersized connectors are upgraded. J7 goes to Micro-Fit 3.0, and the trunk port uses a locking connector.
- Trunk port on the board (locking Micro-Fit, CAN + switched 12 V), with the right screen's termination on a jumper.
- Slot power: a separate AUX 12V input with its own protection and return, on pins 25–30 of every slot (was: a 4-pin
  Micro-Fit at each bay).
- J6 is a general serial port (center UART2, 3.3 V: 3V3, TX, RX, GND), not a GPS connector. GPS goes on a card.

## Expansion cards

What the slots are for, as planned so far. Each card has its own MCU and CAN transceiver and speaks the channel
protocol (`docs/protocols/can_channels.md`). **Every card has a programming header** reachable in its slot: the chips
come from assembly blank (see [Programming](accessory_ports.md#programming-every-card)).

| Card | Slot | MCU | What it does | Uses |
| --- | --- | --- | --- | --- |
| **Audio** (`AudioCarrier/`, drawn) | 1 (its 28.5 mm RCA block needs the open end) | ESP32-PICO-MINI-02 | Bluetooth A2DP / HFP, four line outputs, mic / aux input | 5 V, CAN, I2S master on links A (to slot 2), drives the audio bus |
| **AI voice** | 2 (pairs with the audio card) | ESP32-S3 (or similar) | Listens to the mic, talks back through the speakers | 5 V, CAN, I2S slave on links A: hears LINK 2 (mic), speaks on LINK 3, LINK 4 MCLK if its codec needs it |
| **GPS** ([spec](gps_card.md)) | any | STM32G0B1 (no radio) | Reads a remote GPS puck on a 4-pin cable (same pinout as J6) and sends position, speed, heading, fix and UTC on CAN `0x222`–`0x226`, node `0x05` | 5 V, CAN |
| **LTE gateway** ([spec](lte_card.md)) | any | ESP32-S3 + an A7670 LTE Cat-1 core board on sockets | Internet for the dash: a Wi-Fi access point the center joins on its own (NET gateway, `can_channels.md`), node `0x06`. Later: uploads trips / GPS tracks it hears on CAN | 5 V (ESP32, CAN), AUX 12 V (the LTE board's peaks), CAN |
| **Output / relay** | any | ESP32-C3 | Lights, fans, accessory power | AUX 12 V, CAN, LAMP / REV |

- **GPS and LTE do not talk to each other directly.** CAN is a broadcast bus: the GPS card sends its position once and
  the center, the LTE card and anything else that wants it all hear it. Neither card needs to know the other exists.
- **Online work goes over CAN where it can.** The LTE card does the lookups itself (street name, weather, time zone,
  SMS) and broadcasts the results on the ONLINE channel: a few dozen bytes each. Only bulk data (map tiles, screen
  firmware) needs Wi-Fi, from the LTE card's access point, which is off until it is wanted; CAN then tells the center
  where it is and its password, so nothing is typed in.
- **J6 (serial)** stays for a bench GPS module or any serial device on the center screen until the GPS card exists.

