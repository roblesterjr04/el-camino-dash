# Accessory ports and carrier boards

**Status: planned.** This is the target for the next DigiDash board revision. The current board still has three
ESP32-DevKitC slots (U3 / U11 / U12) and the console header J17 (`docs/protocols/can_bus.md`).

Accessories no longer plug into slots shaped for one module. The DigiDash board offers **ports**, and every
accessory is a **carrier board** with its own MCU and CAN transceiver, in whatever shape suits it. A port carries only
power and the CAN bus, so the main board never needs to know what is plugged in.

There are two kinds of port with the same bus on them:

| Port | Connector on the DigiDash board | For |
| --- | --- | --- |
| **Header position** | 2×5 shrouded header, 2.54 mm, plus two M3 standoff holes | A carrier stacked on the board, or connected by a short ribbon |
| **Cable port** | JST GH 4-pin, locking, on the board edge | Off-board devices on a thin cable (the console) |

## Header position

### Pinout

Looking down on the DigiDash board (and on a carrier stacked on it, which faces the same way):

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
- **Standoffs**: M3, carrier above the DigiDash board, both boards face up (nothing is mirrored, unlike the screens,
  which plug face down). The board-to-board height follows from the parts: a boxed header and an 8.5 mm female
  header give about 11 mm. Check it against the parts on hand before ordering standoffs.
- **Envelope**: a carrier may be any shape that clears the neighbouring positions. The space per position and the
  height limit under the enclosure are set with the board revision.

### Ribbon

A carrier can also sit elsewhere on a 10-way IDC ribbon from a header position. Same pinout, keyed plugs both ends.
Keep it under 30 cm (it is a stub on the bus, see below).

## Cable port

JST GH 4-pin vertical (BM04B-GHS-TBT), locking, on the board edge. The pinout is the DroneCAN / Pixhawk one, so
off-the-shelf GH CAN cables, splitters and terminator plugs fit:

| GH pin | Signal |
| --- | --- |
| 1 | +5 V (0.5 A resettable fuse) |
| 2 | CANH |
| 3 | CANL |
| 4 | GND |

The **console** (ESP32-S3, SSD1322 OLED, encoders; `docs/protocols/can_bus.md`) uses a cable port. It is not
special: any off-board device can use one, and the console would also work from a header position on a ribbon.
The board revision should have two cable ports. The current J17 (JST PH, 1 +5 V, 2 GND, 3 CANL, 4 CANH) is replaced
by this one; the pin order changes.

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

Without slot straps, a carrier's node number comes from its own firmware: each kind of accessory has a default, and a
jumper or setting on the carrier moves it when two of the same kind are fitted. Every node heartbeats on
`0x700 + node` once a second, so the center sees what is on the bus.

| Node | Message range | Default for |
| --- | --- | --- |
| `0x10` | `0x400`–`0x43F` | audio bridge |
| `0x11` | `0x440`–`0x47F` | (free) |
| `0x12` | `0x480`–`0x4BF` | (free) |
| `0x13` | `0x4C0`–`0x4FF` | console |
| `0x14`–`0x1B` | `0x400 + 0x40·(node − 0x10)`, up to `0x6FF` | further accessories |

The ranges are the same as today's (slot 0 = `0x400`, console = `0x4C0`); what changes is who picks the number. When
the board is reworked the console moves from node `0x04` to `0x13` (it has no firmware yet), and the audio bridge
stops reading the slot straps and uses `0x10` unless told otherwise.

## Choosing the MCU for a carrier

| Carrier | MCU | Why |
| --- | --- | --- |
| Small sensor or relay nodes | ESP32-C3 or C6 | CAN controller (TWAI) built in, cheap, small |
| Anything with a UI | ESP32-S3 | More pins and RAM, USB; the console |
| Bluetooth audio | classic ESP32 (ESP32-DevKitC-32) | The only one with Bluetooth Classic (A2DP / AVRCP) |
| Avoid | RP2040 | No CAN controller: needs an MCP2515 or software CAN |

Each carrier has one CAN transceiver: MCP2562 (5 V VDD, 3.3 V VIO, STBY low), as on the DigiDash board today.

## Example: the audio carrier

What slot 0 does today, as a carrier on a header position:

- ESP32-DevKitC-32 (the HiLetgo board) in 2×19 sockets, MCP2562 on GPIO32 (TX) / GPIO33 (RX, through 1 kΩ).
- The two MAX98357A amplifiers (today's J14 / J15), with the speaker terminals on the carrier.
- A 12 V → 5 V buck for the amplifiers from the header's +12 V: at full volume they draw more than the 0.5 A port
  fuse allows at 5 V. The DevKit runs from the port's +5 V (its own 3.3 V regulator; its 3V3 pin stays unconnected).
- Node `0x10`.
