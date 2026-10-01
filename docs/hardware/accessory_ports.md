# Accessory ports and carrier boards

## Rev 2 (`DigiDash2/`): card slots

Rev 2 of the DigiDash board (`docs/hardware/digidash_rev2_plan.md`) has **six card slots** instead of bays. A card is
a board that **stands on edge** in a 2×15 socket on the back of the main board, like a PC expansion card. It brings its
own connectors out on its top edge, where the case puts them. The cable ports and the trunk port stay as below.

### At a glance

| Item | Rev 2 |
| --- | --- |
| Slots | **Six**, J31–J36 = slot 1–6, in a row on the back at the left, **20.32 mm apart** (x 20 … 121.6 in board coordinates), sockets centred at y 53 |
| Socket | 2×15 female, 2.54 mm, vertical, 8.5 mm insulator: Sullins **PPTC152LFBN-RC** or equal. Footprint `DigiDash2:Slot_Socket_2x15_P2.54mm_Vertical` (see [Numbering](#numbering)) |
| Card | **80 × 40 mm** (provisional: the case sets it; one constant in each generator), 1.6 mm. A right-angle 2×15 male header (2.54 mm, KiCad `PinHeader_2x15_P2.54mm_Horizontal`) centred on its bottom edge, the header's plastic body on the edge, so the pins stand 6 mm past it |
| Parts side | Every card's parts face the same way, toward slot 1 (−x). Up to **15 mm** tall. **Slot 1** is at the end of the row and faces off it: no limit, so it is the slot for a card with tall parts (the audio card's RCA block is 28.5 mm) |
| Back side | Up to **2.5 mm** including solder tails (it faces the next card's parts) |
| Under the cards | The main board keeps parts under about 7 mm in each card's envelope (the card's bottom edge sits about 8.5 mm up). `gen_pcb.py` keeps the tall parts out |
| Links | **Neighbour pairs**: slots 1–2 share link set A, 3–4 set B, 5–6 set C. Nothing else on the main board touches them |
| AUX 12 V | From J13 (own fuse at the fuse box) and the board's 10 A fuse, on pins 25–30 of every slot. The old bay power plugs (J14 / J15) are gone |
| Cable ports | **Four**: J17, J22, J23, J24 (JST PH 4-pin, pinout below), on the top edge |
| Trunk port | **J25**, a 4-pin Micro-Fit 3.0 on the bottom edge (moved to x 136, clear of the cards) that extends the bus itself to the rear of the car: 1 CANH, 2 CANL (twisted pair), 3 switched 12 V (1.1 A PTC), 4 GND, with its own ESD clamp. The right-end 120 Ω is switched by SW1-1: turn it off when a trunk cable is fitted, and fit 120 Ω at the far end of that cable |
| Switched 12 V | Pin 8 of every slot, the trunk port and the camera come from a P-FET switched by the ignition |

### Slot pinout

| Pins | Odd | Even |
| --- | --- | --- |
| 1 / 2 | +5 V | +5 V |
| 3 / 4 | GND | GND |
| 5 / 6 | CANH | CANL |
| 7 / 8 | GND | switched 12 V |
| 9 / 10 | +3.3 V | constant 12 V |
| 11 / 12 | slot ID | WAKE |
| 13 / 14 | LAMP (dash-light level) | REV (reverse) |
| 15 / 16 | LINK 0 | LINK 1 |
| 17 / 18 | LINK 2 | LINK 3 |
| 19 / 20 | GND | GND |
| 21 / 22 | AUDIO L | AUDIO R |
| 23 / 24 | AUDIO GND | LINK 4 |
| 25 / 26 | AUX 12 V | AUX 12 V |
| 27 / 28 | AUX 12 V | AUX GND |
| 29 / 30 | AUX GND | AUX GND |

| Signal | On the main board | What a card does with it |
| --- | --- | --- |
| **+5 V** (1, 2) | 0.5 A PTC per slot | Main logic supply. Two pins share the current |
| **CANH / CANL** (5, 6) | The board's bus, ground on both sides of the pair | One MCP2562 (5 V VDD, 3.3 V VIO). **Never terminate** |
| **Switched 12 V** (8) | 0.5 A PTC per slot, on with the key | "Key on", relay coils, small 12 V loads |
| **+3.3 V** (9) | 0.1 A PTC per slot, from the board's 1 A LDO | Small cards (an ID chip, a sensor) can skip their own regulator. Not for an ESP32 with its radio on |
| **Constant 12 V** (10) | 0.2 A PTC per slot, from the battery after the reverse-polarity FET | Key-off jobs (GPS hot start, a clock alarm, logging after shutdown). A card that uses it sleeps below about 1 mA with the key off |
| **Slot ID** (11) | A resistor to GND per slot: 1k, 3.3k, 6.8k, 12k, 22k, 47k (slot 1 … 6) | 10k pull-up to the card's 3.3 V, read on an ADC: about 0.30, 0.82, 1.34, 1.80, 2.27, 2.72 V. The card knows its slot, its link partner (slot 1 ↔ 2, …) and can derive its CAN node number |
| **WAKE** (12) | Shared by all slots, 100k to GND. It lifts the 5 V buck's EN through a diode (like the sensor pack's PWR_HOLD) and goes to the ATmega2560 (pin 88, ADC9 / PCINT17) | To start the dash with the key off, drive it **high (3.3 V) through a diode**; never drive it low. The 2560 wakes, sees why, and holds the power with PWR_HOLD while it needs it |
| **LAMP** (13) | J7 pin 6 (the dash-light feed, a rheostat voltage) through 100k / 27k, clamped: 0–3.1 V | Dim LEDs or a display with the dash lights (ADC) |
| **REV** (14) | J9 pin 1 (reverse lamp) through 100k / 27k, clamped: high = in reverse | Parking sensors, a rear camera, a beeper |
| **LINK 0–4** (15–18, 24) | Bare traces between the two slots of a pair only (A: 1–2, B: 3–4, C: 5–6) | Whatever the two cards agree: I2S, SPI, UART, I2C, sync pulses. See [Link rules](#link-rules) |
| **AUDIO L / R** (21, 22) | Shared by all slots, bare | Line level. One card drives it (the audio card), any card listens |
| **AUDIO GND** (23) | Shared; joins GND only at net tie NT2 | The reference for AUDIO L / R. The driving card ties it to its ground through about 10 Ω; listeners use it as their input reference |
| **AUX 12 V / GND** (25–30) | Three pins each, from the AUX input (10 A board fuse, no per-slot fuse). AUX GND meets GND only at NT1 | High-current loads: amplifiers, output nodes. About 5–6 A per card (2 A per contact), all slots together under the 10 A fuse |

### Link rules

- The links join the **two slots of a pair** and nothing else, so two pairs can each run their own link at the same
  time. Put two cards that need to talk directly in the same pair: slots 1–2, 3–4 or 5–6.
- **High impedance until the partner is known.** A card leaves every link pin undriven (or behind a 3-state buffer)
  until it has seen its partner on CAN (NET ANNOUNCE from the partner slot's card, which it can tell from the slot ID
  in the ANNOUNCE) and both agree on the use. That way a card never fights an unknown neighbour.
- Drivers put about 33 Ω in series at the pin. Inputs have a weak pull (100k) so an empty partner slot reads idle.
- I2S between two cards uses all five: LINK 0 BCK, LINK 1 WS, LINK 2 data from the clock master, LINK 3 data to it,
  LINK 4 MCLK (for codecs that need it). The clock master is the card that owns the DACs (the audio card).

### Numbering

KiCad's stock female-socket footprints number their rows mirrored, for two boards mated face to face. A card standing
in the slot meets it differently, so the slot uses its own footprint, generated by `DigiDash2/generate/gen_footprints.py`:
its pads are numbered like a pin header (pad 2 at +2.54 mm from pad 1, the pads counting down the rows). With the
socket on the back and turned as `gen_pcb.py` places it (pin 1 at the bottom, the even row toward +x), **each socket
pad meets the same-numbered pin** of a card's `PinHeader_2x15_P2.54mm_Horizontal`, placed on the card with its pins
off the bottom edge, pad 2 below pad 1 and pad 3 to the left of pad 1 (seen from the card's parts side, top edge up,
pin 1 is at the right). `AudioCarrier/generate/gen_pcb.py` shows how; both generators check the orientation from the
pads and stop if it is wrong.

### Parts

| Part | On | Example part number | Notes |
| --- | --- | --- | --- |
| 2×15 female socket, 2.54 mm, vertical, 8.5 mm insulator | main board (`Slot_Socket_2x15_P2.54mm_Vertical`, back) | Sullins **PPTC152LFBN-RC** | 3 A per contact |
| 2×15 male header, 2.54 mm, right angle, dual row | card's bottom edge (`PinHeader_2x15_P2.54mm_Horizontal`) | any with 6 mm mating pins | The plastic body sits on the card's edge |

The rest of this document describes rev 1 (`DigiDash/`) and its 2×5 header positions, which rev 2 no longer has.

## Rev 1 (`DigiDash/`)

**Status: in the DigiDash schematic and PCB, placed and autorouted (Freerouting 2.1, 45° traces, no 90° corners away from pads).** The three ESP32-DevKitC slots (U3 / U11 / U12),
their transceivers and the amplifier sockets J14 / J15 are gone. On the board now:

| Part | What | Placed at (back, mm) |
| --- | --- | --- |
| J18, J19, J20, J21 | header positions 1–4 (`DigiDash_Accessory:Port_Header_2x05`) | (−74, 28.5), (−16, 28.5), (−84, 74), (−84.7, 97.3) |
| J17, J22 | cable ports 1–2 (JST PH S4B-PH-K, through-hole) | (45.65, 17.95), (65.65, 17.95), top edge |
| F1–F8 | 0.5 A PTCs (MF-R050): +5 V and +12 V for each header position | beside their position |
| F9, F10 | 0.5 A PTCs: +5 V for each cable port | beside J17 / J22 on the top edge |
| D2 | NUP2105L CAN ESD clamp (SOT-23) | (34.5, 59.7) |

Everything is placed from the old slot areas outward, clear of other parts' courtyards and of existing copper, and with
a 50 × 26 mm carrier envelope kept free of back-side parts at each header position. The cable ports sit on the top edge (side entry, the socket overhangs the edge by about 1.3 mm), each with its PTC beside it and silkscreen pin labels (5V, H = CANH, L = CANL, G = GND) and a name (CONSOLE, SPARE) on the back. The screen headers will move once
the screens are laid out with the bezel, so expect to re-route then: run **Update PCB from Schematic**, refill the
zones (**B**) and re-run the autorouter. All tracks were ripped up and rerouted from scratch when the cable ports moved.

Accessories no longer plug into slots shaped for one module. The DigiDash board offers **ports**, and every
accessory is a **carrier board** with its own MCU and CAN transceiver, in whatever shape suits it. A port carries only
power and the CAN bus, so the main board never needs to know what is plugged in.

There are two kinds of port with the same bus on them:

| Port | Connector on the DigiDash board | For |
| --- | --- | --- |
| **Header position** | 2×5 shrouded header, 2.54 mm, plus two M3 standoff holes | A carrier stacked on the board, or connected by a short ribbon |
| **Cable port** | JST PH 4-pin through-hole, side entry, on the board edge | Off-board devices on a thin cable (the console) |

## Header position

### Where they go

The front of the DigiDash board belongs to the three screens (they plug face down onto J5 / J1 / J4), so **carriers
mount on the back**, flat, with their parts facing away from the board into the dash cavity. The back already carries
other plug-in boards that are **not** accessory ports and stay as they are: the TVP5150 video module (J13, parallel video
straight into the center's camera pins) and the FRAM board (FRAM1, on the ATmega). They, and the tall or
cable-bearing parts, are keep-outs for carrier positions:

| Area on the back (board coordinates, mm) | First revision | Now |
| --- | --- | --- |
| x −97…−51, y 16…41 | U11 DevKit (slot 1) | header position J18 |
| x −39…7, y 16…41 | U12 DevKit (slot 2) | header position J19 |
| x −112…−55, y 62…112 | U3 DevKit (slot 0, audio) and the J14 / J15 amplifier sockets | header positions J20, J21 |
| x ≈112…125, y 71…94 and toward the right edge | J13 TVP5150 video module | keep-out |
| x ≈132…138, y 22…39 | FRAM board | keep-out |
| x 16…34, y 80…110 | LM2576 (TO-220, standing), L1, C3 | keep-out (tall) |
| x 60…89, y 60…66 | PTC thermistors TH1–TH4 (standing, ~17 mm) | keep-out (tall) |
| bottom edge, y ≈ 111 | J6 / J7 / J9 / J10 harness connectors | keep clear for the plugs and cables |
| left edge | J11 / J12 RCA jacks | keep clear for the plugs |
| x 142…155, y 22…41 | J2 AVR ISP, J3 sensors | keep reachable |

**Depth**: the DevKits in their sockets already stand about 20 mm off the back, as do the regulator and thermistors.
A flat carrier (about 11 mm stack plus ordinary parts) fits in that same depth, so carriers add nothing to what the
board already needs behind the dash. Standing (perpendicular) carriers are not part of the standard; if one is ever
needed it can use a right-angle socket and an L-bracket on the same two M3 holes.

### Pinout

Looking at the back of the DigiDash board (and at a carrier's component side, which faces the same way):

| Pin | Signal | | Pin | Signal |
| --- | --- | --- | --- | --- |
| 1 | +5 V | | 2 | +5 V |
| 3 | GND | | 4 | GND |
| 5 | CANH | | 6 | CANL |
| 7 | GND | | 8 | +12 V switched |
| 9 | reserved (no connect) | | 10 | reserved (no connect) |

- **+5 V**: from the DigiDash 5 V rail through a resettable fuse per position (0.5 A hold). Two pins share the current.
- **+12 V switched**: the ignition feed after the board's input protection, through its own 0.5 A resettable fuse per
  position. It is there for carriers that switch relays or need more power than 0.5 A at 5 V (they regulate it down
  themselves). It is on whenever the key is on, like everything else.
- **CANH / CANL**: the board's bus, side by side and next to ground (the best a ribbon can do for a pair).
- **9 / 10**: reserved. Carriers leave them unconnected, so they can be given a job later without breaking anything.
- Nothing else is on the header: no I2S, SPI or UART. Anything a carrier needs beyond CAN stays on the carrier.

### Mechanical

Footprints: [`DigiDash/DigiDash_Accessory.pretty`](../../DigiDash/DigiDash_Accessory.pretty)
(`Port_Header_2x05` on the DigiDash board, `Carrier_Header_2x05` on a carrier).

- The DigiDash board has a **shrouded (boxed) 2×5 header**. It takes either a carrier's 2×5 **female header**
  (fitted on the carrier's underside) or a **keyed IDC ribbon plug**.
- Coordinates are relative to pin 1; pin 2 is at +2.54 mm X, and pins 3, 5, 7, 9 run down +Y in 2.54 mm steps.
  The header's centre is at (1.27, 5.08).
- **Two M3 holes** (3.2 mm, plated, tied to GND on both boards):
  - **H1** at (1.27, −15.24): 20.32 mm from the header centre, beyond pin 1.
  - **H2** at (8.89, 25.40): 20.32 mm the other way, **offset 7.62 mm toward the even-pin side**.

  The pattern is deliberately not symmetric. A shrouded header takes a plain female header either way round, and
  turned 180° a carrier would put +12 V on its ground pin. With these holes it cannot be screwed down turned around.
- **Standoffs**: M3, carrier behind the DigiDash board with its component side facing away from it and its female
  header on the side facing the board. Seen from behind, pin 1 is in the same place on both. In KiCad the
  `Port_Header_2x05` footprint is placed on the back layer (it flips; the asymmetric holes still stop a turned carrier)
  and `Carrier_Header_2x05` goes on the carrier as drawn (socket on the carrier's bottom side). The board-to-board height follows from the parts: a boxed header and an 8.5 mm female
  header give about 11 mm. Check it against the parts on hand before ordering standoffs.
- **Envelope**: a carrier may be any shape that clears the neighbouring positions. The space per position and the
  height limit under the enclosure are set when the board is laid out against the bezel.

### Ribbon

A carrier can also sit elsewhere on a 10-way IDC ribbon from a header position. Same pinout, keyed plugs both ends.
Keep it under 30 cm (it is a stub on the bus, see below).

## Cable port

JST PH 4-pin, side entry (S4B-PH-K), through-hole so it hand-solders, on the board edge. It is not locking. The pin
order is the DroneCAN / Pixhawk one (1 +5 V, 2 CANH, 3 CANL, 4 GND), but the PH connector is not GH-compatible, so
the cable is a PH-to-PH one you make up:

| PH pin | Signal |
| --- | --- |
| 1 | +5 V (0.5 A resettable fuse) |
| 2 | CANH |
| 3 | CANL |
| 4 | GND |

The **console** (ESP32-S3, SSD1322 OLED, encoders; `docs/protocols/can_bus.md`) uses a cable port. It is not
special: any off-board device can use one, and the console would also work from a header position on a ribbon.
The board has two cable ports, J17 (the console) and J22 (spare). J17 was a JST PH with 1 +5 V, 2 GND, 3 CANL, 4 CANH
on the first revision; the pin order changed when it became a GH and stayed changed when it went back to PH.

A device that needs more than 0.5 A, or +12 V, uses a header position instead (or a ribbon from one).

## The bus

- **500 kbit/s**, termination (120 Ω) only at the two ends of the bus **on the DigiDash board**. Carriers and cable
  devices never terminate, so any of them can be unplugged and the bus keeps working.
- Every port is a stub off the board's bus. Keep each stub (ribbon or cable) **under about 30 cm**. A device that has
  to be farther away either gets two connectors so the bus passes through it (it then sits on the bus instead of on
  a stub), or the whole bus runs at 250 kbit/s (`ELC_CAN_BITRATE_KBPS=250` on every node), which doubles what it
  tolerates.
- One CAN ESD protection diode on the DigiDash board. Carriers fit their own if they have long cables.

## Power budget

The DigiDash 5 V rail (LM2576T-5, 3 A) also feeds the three displays. Budget about **1 A in total at 5 V for all
ports** until the displays' draw is measured; the per-port fuses stop one carrier from taking the dash down.
Carriers that need more (audio amplifiers) take +12 V from their header position and regulate it themselves.

## Node numbers and CAN IDs

What a carrier says on the bus to use the dash's alerts, sensors, menu, input and sounds is the channel protocol:
[`docs/protocols/can_channels.md`](../protocols/can_channels.md). The ranges below are each node's private IDs.

On rev 1 a carrier's node number comes from its own firmware: each kind of accessory has a default, and a jumper or
setting on the carrier moves it when two of the same kind are fitted. On rev 2 a card can also read its slot ID (pin 11)
and offset its default by the slot, so two identical cards in different slots never clash. Every node heartbeats on
`0x700 + node` once a second, so the center sees what is on the bus.

| Node | Message range | Default for |
| --- | --- | --- |
| `0x10` | `0x400`–`0x43F` | audio bridge |
| `0x11` | `0x440`–`0x47F` | (free) |
| `0x12` | `0x480`–`0x4BF` | (free) |
| `0x13` | `0x4C0`–`0x4FF` | console |
| `0x14`–`0x1B` | `0x400 + 0x40·(node − 0x10)`, up to `0x6FF` | further accessories |

The ranges are the same as the first revision's (slot 0 = `0x400`, console = `0x4C0`); what changed is who picks the
number. The console is node `0x13` (`ELC_CAN_NODE_CONSOLE`; it was `0x04` before it had firmware). The audio bridge
still reads its two straps (GPIO13 / GPIO23, pull-ups, grounded = 1): on a carrier they are simply left open (node
`0x10`), or a jumper to GND picks `0x11` / `0x12`.

## Choosing the MCU for a carrier

| Carrier | MCU | Why |
| --- | --- | --- |
| Small sensor or relay nodes | ESP32-C3 or C6 | CAN controller (TWAI) built in, cheap, small |
| Anything with a UI | ESP32-S3 | More pins and RAM, USB; the console |
| Bluetooth audio | classic ESP32 with PSRAM (ESP32-DevKitC-VE / WROVER-E, or the carrier's PICO-MINI-02-N8R2) | The only one with Bluetooth Classic (A2DP / AVRCP). PSRAM: with a phone streaming, a PSRAM-less ESP32 has ~32 KB of RAM left, too little for the album-art copies |
| Avoid | RP2040 | No CAN controller: needs an MCP2515 or software CAN |

Each carrier has one CAN transceiver: MCP2562 (5 V VDD, 3.3 V VIO, STBY low), as on the DigiDash board today.

## Example: the audio carrier (rev 1)

Rev 2's audio card (`AudioCarrier/`, slot 1) is described in [`AudioCarrier/README.md`](../../AudioCarrier/README.md).
What slot 0 did on rev 1, as a carrier on a header position:

- ESP32-DevKitC-VE (ESP32-WROVER-E, 8 MB PSRAM; same outline and 2×19 pinout as the DevKitC-32, which it replaces) in
  2×19 sockets, MCP2562 on GPIO32 (TX) / GPIO33 (RX, through 1 kΩ). GPIO16 / 17 go to the PSRAM.
- The two MAX98357A amplifiers (today's J14 / J15), with the speaker terminals on the carrier.
- A 12 V → 5 V buck for the amplifiers from the header's +12 V: at full volume they draw more than the 0.5 A port
  fuse allows at 5 V. The DevKit runs from the port's +5 V (its own 3.3 V regulator; its 3V3 pin stays unconnected).
- Node `0x10`.
