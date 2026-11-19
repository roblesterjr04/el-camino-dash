# Designing your own main board

The El Camino Dash is built as a **platform**. The cards (sensor, audio, radio, GPS, LTE, and anything you design),
the console and the round screens never need to know which car they are in. Only the main board does: its outline,
its screen spacing, its connectors to the harness. [`DigiDash2/`](../../DigiDash2/) is the main board for a 1964
El Camino. If your car, boat, kit car or bench needs a different shape, a different number of screens or a different
number of slots, design your own main board to the interface on this page, and every existing card plugs into it
unchanged.

This page is the contract. It says what a main board **must** provide, what it **should** provide, and what it may
leave out. The reference values come from DigiDash2 (`DigiDash2/generate/gen_sch.py` is the source of truth for every
net; [`accessory_ports.md`](accessory_ports.md) is the slot specification).

Contents:

- [What the main board does](#what-the-main-board-does)
- [The card slot](#the-card-slot)
- [Slot pins, one by one](#slot-pins-one-by-one)
- [The CAN bus on the main board](#the-can-bus-on-the-main-board)
- [Slot IDs and node numbers](#slot-ids-and-node-numbers)
- [Power](#power)
- [The I2S rail and the analog audio bus](#the-i2s-rail-and-the-analog-audio-bus)
- [Screens](#screens)
- [Cable ports, trunk port and the debug tap](#cable-ports-trunk-port-and-the-debug-tap)
- [Mechanics of the slot](#mechanics-of-the-slot)
- [Minimum main boards](#minimum-main-boards)
- [Bring-up checklist](#bring-up-checklist)
- [Designing a card](#designing-a-card)

## What the main board does

```
                 ┌────────────────────────────── your main board ──────────────────────────────┐
 battery 12 V ──►│ reverse-polarity FET ─► constant 12 V ──────────────────────► slot pin 10   │
 ignition ──────►│ high-side switch ─────► switched 12 V ──────────────────────► slot pin 8    │
                 │ 5 V buck (EN from ignition OR WAKE) ─► +5 V ─► PTC per slot ─► slot pins 1,2 │
                 │ 3.3 V LDO ─► PTC per slot ──────────────────────────────────► slot pin 9    │
 AUX 12 V ──────►│ fuse, reverse FET, TVS ─► AUX 12 V / AUX GND ───────────────► slot 25–30    │
 dash lights ───►│ divider + clamp ─► LAMP level ──────────────────────────────► slot pin 13   │
 reverse lamp ──►│ divider + clamp ─► REV level ───────────────────────────────► slot pin 14   │
                 │                                                                              │
                 │ CAN: 120 Ω ── screen ── slot ── slot ── … ── cable port ── screen ── 120 Ω  │
                 │ one ID resistor per slot ───────────────────────────────────► slot pin 11   │
                 │ WAKE line (100k to GND, diode-OR into the buck EN) ─────────► slot pin 12   │
                 │ I2S rail, analog audio bus: bare traces bused to every slot                  │
                 └──────────────────────────────────────────────────────────────────────────────┘
```

The main board has **no microcontroller of its own**. Everything with firmware is a node on the CAN bus: the screens
(plugged onto headers), the cards (in slots), the console (on a cable). The main board is power, the bus, a few
vehicle levels and the connectors. That is what makes it cheap to redraw for a new vehicle.

## The card slot

A slot is a **2×15 female socket, 2.54 mm pitch, vertical** (Sullins PPTC152LFBN-RC or equal, 8.5 mm insulator,
3 A per contact). A card is an 80 × 60 mm board that stands on edge in it, with a right-angle 2×15 male header on its
bottom edge.

### Pinout

| Pins | Odd | Even |
| --- | --- | --- |
| 1 / 2 | +5 V | +5 V |
| 3 / 4 | GND | GND |
| 5 / 6 | **CANH** | **CANL** |
| 7 / 8 | GND | switched 12 V |
| 9 / 10 | +3.3 V | constant 12 V |
| 11 / 12 | slot ID | WAKE |
| 13 / 14 | LAMP (dash-light level) | REV (reverse) |
| 15 / 16 | I2S BCK | I2S WS |
| 17 / 18 | I2S MIC | I2S RET |
| 19 / 20 | GND | GND |
| 21 / 22 | AUDIO L | AUDIO R |
| 23 / 24 | AUDIO GND | I2S MCLK |
| 25 / 26 | AUX 12 V | AUX 12 V |
| 27 / 28 | AUX 12 V | AUX GND |
| 29 / 30 | AUX GND | AUX GND |

The layout is deliberate:

- **Power at the ends, signals in the middle.** The logic supplies (1–4) are at one end and the high-current AUX
  supply (25–30) at the other, so a card can route each away from the other.
- **CANH / CANL are a pair (5, 6) with ground on both sides** (pins 3/4 and 7). The pair crosses the connector
  side by side and keeps its return next to it.
- **The I2S rail (15–18, 24) is fenced by ground** (pins 19/20) from the analog audio bus (21–23), which has its own
  ground (23).
- **AUX GND is separate from GND.** High load currents return on their own pins and meet logic ground only at one
  point on the main board.

### Numbering, and why it matters

**Socket pin n must meet the card's header pin n.** That sounds obvious, but a 2×15 right-angle header seen from the
socket side is mirrored. If you get it wrong, the slot's 5 V lands on the card's AUX GND and AUX 12 V on its GND and
CAN pins. DigiDash2 made this mistake once and fixed it on 2026-10-19.

- On the card: KiCad's stock `PinHeader_2x15_P2.54mm_Horizontal`, pins off the bottom edge, plastic body on the edge.
  Seen from the card's parts side with the top edge up, **pin 1 is at the right**, and the even row is the lower one.
- On the main board: KiCad's stock female socket numbering, placed so that each pad meets the same-numbered pin of
  that header. DigiDash2 uses its own footprint `Slot_Socket_2x15_P2.54mm_Vertical` (`DigiDash2/generate/gen_footprints.py`);
  BenchBoard uses the stock socket turned so that pin 1 is west and pad 2 south.
- **Check it from the pads, not from the silkscreen.** Both DigiDash2's and AudioCarrier's `gen_pcb.py` compute
  the pad positions and stop if the orientation is wrong. Do the same: put a card footprint and a slot footprint in
  one KiCad file, in their mated positions, and check that every pad pair has the same number.

[`accessory_ports.md`, Numbering](accessory_ports.md#numbering) has the full derivation.

## Slot pins, one by one

| Pin | Signal | Required? | What the main board provides | What a card may assume |
| --- | --- | --- | --- | --- |
| 1, 2 | +5 V | **must** | 5 V through a **0.5 A PTC per slot** (MF-R050 or equal) | Up to about 0.5 A at 5 V. Its main logic supply; it makes its own 3.3 V |
| 3, 4, 7, 19, 20 | GND | **must** | Logic ground, solid plane | |
| 5, 6 | CANH, CANL | **must** | The bus, as a differential pair; see [the CAN bus](#the-can-bus-on-the-main-board) | The bus is terminated by the main board. **A card never terminates** |
| 8 | switched 12 V | should | Battery through an ignition-switched high-side P-FET, **0.5 A PTC per slot** | "Key on" sense and small 12 V loads (a relay coil). May be absent: a card must not depend on it to run |
| 9 | +3.3 V | should | From a board LDO, **0.1 A PTC per slot** | Enough for an ID chip or a small sensor. **Not** for an ESP32 with its radio on |
| 10 | constant 12 V | should | Battery after the reverse-polarity FET, **0.2 A PTC per slot** | Key-off work (GPS hot start, a clock). A card using it must sleep below about 1 mA with the key off |
| 11 | slot ID | **must** | A resistor to GND, a different value per slot; see [slot IDs](#slot-ids-and-node-numbers) | Read it with a 10k pull-up to the card's 3.3 V on an ADC pin |
| 12 | WAKE | should | Shared by all slots, **100k to GND**, diode-ORed into the 5 V regulator's enable | A card may drive it **high only** (about 3.3 V, through its own diode) to keep the dash powered after key-off. Never drive it low |
| 13 | LAMP | optional | Dash-light (rheostat) voltage through 100k / 27k, 100 nF, clamped to 0–3.3 V (BAT54S) | An ADC level 0–3.1 V. 0 V when not provided |
| 14 | REV | optional | Reverse-lamp voltage, same divider and clamp: high = in reverse | A digital or ADC level. Low when not provided |
| 15–18, 24 | I2S rail | optional | Five bare traces bused to every slot, nothing else on them | See [the I2S rail](#the-i2s-rail-and-the-analog-audio-bus) |
| 21, 22, 23 | AUDIO L, R, AUDIO GND | optional | Bused to every slot; AUDIO GND joins GND **only** at one net tie | Line level, one driver (the audio card) |
| 25–27 | AUX 12 V | optional | A separate fused 12 V input (DigiDash2: 5 A fuse, reverse P-FET, SMCJ33A TVS, 100 µF), **no per-slot fuse** | Higher-power 12 V loads, about 3 A for all slots together |
| 28–30 | AUX GND | optional | AUX return; joins GND **only** at one net tie | |

"Optional" means a card that needs the signal will not work without it, but nothing is damaged. Tie unused
optional pins as follows, so every card sees a defined level:

- LAMP and REV: 10k to GND (reads "lights off", "not in reverse").
- I2S rail and audio bus: leave the traces bused between slots even if no audio card is planned; they cost nothing.
- AUX 12 V: leave open. A card that needs AUX (the LTE card) checks for it.
- switched 12 V, constant 12 V, +3.3 V: leave open if not provided. The audio card, sensor card and others run from +5 V.

## The CAN bus on the main board

Every node in the system speaks **classic CAN at 500 kbit/s with 11-bit IDs** on one bus. The main board owns the
physical bus. The protocol itself is in [`docs/protocols/can_guide.md`](../protocols/can_guide.md).

### Topology

```
  120 Ω                                                                                 120 Ω
   ┌┴┐                                                                                   ┌┴┐
───┴─┴──●────────●────────●────────●──── … ────●────────●────────●────────●──────────────┴─┴───
        │        │        │        │           │        │        │        │
     LEFT     slot 1   slot 2   slot 3      slot 6   cable     CENTER   RIGHT ── (SW1) ── trunk port
    screen                                           ports     screen   screen            to the rear
```

- **One linear bus**, a differential pair routed from end to end, with every node a short **stub** off it.
- **120 Ω at each physical end, and only there.** DigiDash2 puts one at the left screen's transceiver and one at the
  right screen's, the right one through a slide switch (SW1-1) so it can be turned off when a trunk cable carries the
  bus on to the rear of the car (then fit 120 Ω at the far end of that cable). With the board powered off, CANH to
  CANL must measure **about 60 Ω** whatever is plugged in.
- **Cards, the console and cable devices never terminate.** That is a hard rule of the platform: any card can be
  pulled and the bus is still correct.
- **Stubs short.** On the board, route the slot pins and screen transceivers with stubs of a few centimetres. Off the
  board (cable ports), keep a cable under about **30 cm** at 500 kbit/s. For longer runs either pass the bus through
  the device (two connectors, so it is on the bus rather than a stub) or run the whole bus at 250 kbit/s
  (`ELC_CAN_BITRATE_KBPS=250` in every node's firmware).
- **Routing:** CANH and CANL side by side as a pair (about 120 Ω differential if you can, but at 500 kbit/s and a
  board this size, a tight parallel pair over a solid ground plane is what matters), ground on both sides at each
  slot (the pinout gives you this), no other signal between them.

### Transceivers for the screens

The screens are ESP32-P4 boards with a CAN controller (TWAI) but **no transceiver**, so the main board carries one
per screen:

| Item | DigiDash2 | Notes |
| --- | --- | --- |
| Transceiver | TJA1051T/3 (SOIC-8) per screen (rev 1 used MCP2562) | Any 5 V high-speed CAN transceiver with a separate logic supply (VIO) works |
| VDD | +5 V, 100 nF | |
| VIO | board +3.3 V, 100 nF | The P4's pins are 3.3 V |
| TXD | from header pin 31 (GPIO34), **10k pull-up to 3.3 V** | Holds the bus recessive while the screen boots |
| RXD | to header pin 23 (GPIO36) | |
| STBY / S | to GND | Always on |
| ESD | one NUP2105L on CANH / CANL (and another at the trunk port) | Cards with long cables fit their own |

Every card carries its own transceiver (MCP2562 or TJA1051T/3: 5 V VDD, 3.3 V VIO, standby tied low), so the slot
carries only CANH and CANL, never TX/RX logic signals.

### Bus load and timing

The cluster today uses about 5 % of the bus, plus about 2 % for the core sensors on the SENSOR channel. The budget
for everything, including bulk transfers and album art, is about 70 %. A main board change never changes this; it
only matters if you add many cards that stream. See [the CAN guide, bus budget](../protocols/can_guide.md#bus-budget).

## Slot IDs and node numbers

A card's CAN node number comes from **the slot it is in**, never from its firmware or a jumper. That is what lets two
identical cards sit in one car and a card move between slots without configuration.

| Slot | ID resistor to GND | Volts at the card (10k pull-up to 3.3 V) | Card decodes slot when below | Node number |
| --- | --- | --- | --- | --- |
| 1 | 1 kΩ | 0.30 V | 560 mV | `0x14` |
| 2 | 3.3 kΩ | 0.82 V | 1078 mV | `0x15` |
| 3 | 6.8 kΩ | 1.34 V | 1568 mV | `0x16` |
| 4 | 12 kΩ | 1.80 V | 2035 mV | `0x17` |
| 5 | 22 kΩ | 2.27 V | 2493 mV | `0x18` |
| 6 | 47 kΩ | 2.72 V | 2950 mV | `0x19` |
| none | open | ~3.3 V | — | the card stays off the bus (bench) |

The thresholds are in `firmware/elc_can/include/elc_can_ids.h` (`elc_can_card_slot_from_mv()`). Use 1 % resistors.

**Rules for your board:**

- **At most six slots**, with these six resistor values. The node numbers `0x14`–`0x19` are the slot range; a seventh
  slot would collide with the cable-port devices `0x1A`–`0x1B`.
- **Fewer slots is fine.** A board with two slots fits slot 1's and slot 2's resistors (BenchBoard does exactly this).
  You may also fit, say, slots 1, 3 and 4 if that suits your layout: the numbers only have to be unique on the bus.
- **Every slot gets a different resistor.** Two slots with one value give two cards one node number, and their frames
  collide. (The protocol has the center spot this from two ANNOUNCEs with one number and different unique IDs.)
- **Slot 1 is the audio card's slot by convention** (it is the clock master of the I2S rail, and on DigiDash2 its tall
  RCA block needs the open end of the row). Put slot 1 where a tall card fits.
- **The sensor card ignores the slot ID** and is always node `0x04`. It may go in any slot but slot 1.

## Power

| Rail | DigiDash2 | Sizing guidance for your board |
| --- | --- | --- |
| +5 V | TPS54560B buck, 5 A, EN from the ignition (442k / 90.9k, starts above about 6.5 V, 5.1 V clamp) OR'd with WAKE | Three screens plus six cards: measure the screens' draw with the backlight up, then add 0.5 A per slot at most. A 3 A regulator is too small for six busy slots; 5 A is comfortable |
| +3.3 V | AP7361C-33, 1 A | Feeds the screen transceivers' VIO, the I2C pull-ups and the slots' 0.1 A PTCs |
| Switched 12 V | P-FET high-side switch driven from the ignition | 0.5 A per slot, plus the camera and trunk port |
| Constant 12 V | After the reverse-polarity FET | 0.2 A per slot |
| AUX 12 V | Separate input connector (Mini-Fit Jr), own fuse at the fuse box, 5 A board fuse, reverse P-FET, TVS, 100 µF | 1 mm tracks carry about 3 A. A card needing more brings its own power connector on its top edge |

**WAKE.** The 5 V regulator's enable is the OR of the ignition and the WAKE line. A card that has to finish work
after key-off (the sensor card saving the odometer; a logger) drives WAKE high through a diode, and the dash stays
powered until it lets go. On DigiDash2: WAKE has 100k to GND; a BAT54S diode and a 47k resistor lift the buck's EN
(3.3 V on WAKE gives about 2 V on EN, which starts above 1.2 V). If your regulator's EN behaves differently,
recompute that resistor; the card side of the contract (drive high, about 3.3 V, through a diode) does not change.

**Grounds.** Three grounds meet at defined points: logic GND (planes), AUX GND (net tie NT1) and AUDIO GND (net
tie NT2). Keep AUX load currents and the audio reference out of the logic ground everywhere else.

**Protection** on the battery input: reverse-polarity P-FET and a TVS rated for automotive load dump (DigiDash2 uses
SMCJ33A on AUX). Every slot supply has its own PTC, so one faulty card cannot take the dash down.

## The I2S rail and the analog audio bus

Both are optional. Without them the audio card still plays Bluetooth audio to its own RCA outputs; what you lose is
digital audio between cards (the radio card sending its audio to the audio card, a voice card).

**I2S rail (pins 15–18, 24):** five **bare traces**, bused to every slot, with nothing else on them: no pull-ups,
no series resistors, no termination on the main board. The cards bring their own 33 Ω series resistors and 3-state
buffers. The audio card is the only clock master; one card at a time is granted the RET line over CAN (NET RAIL).

| Pin | Signal | Speed | Routing |
| --- | --- | --- | --- |
| 15 | BCK | 64 fS, about 3 MHz | comfortable |
| 16 | WS | fS, 44.1 or 48 kHz | comfortable |
| 17 | MIC | data | comfortable |
| 18 | RET | data | comfortable |
| 24 | MCLK | 256 fS, 12.288 MHz | the fastest edge: keep it short, over solid ground |

DigiDash2's rail is about 100 mm long with six short stubs. If your slots are far apart (beyond about 150 mm end to
end), keep MCLK over a solid ground plane and consider leaving it off: most cards do not use it.

**Analog audio bus (pins 21–23):** AUDIO L and R are line level, driven by one card (the audio card) and read by any
card. AUDIO GND is their reference: the driving card ties it to its ground through about 10 Ω, and the main board
joins it to logic ground only at one net tie.

Full rules: [`accessory_ports.md`, I2S rail rules](accessory_ports.md#i2s-rail-rules) and
[`can_channels.md`, The I2S rail](../protocols/can_channels.md#the-i2s-rail).

## Screens

The screens are off-the-shelf **Waveshare ESP32-P4-WIFI6-Touch-LCD** round display boards (4C: 4", 720×720; 3.4C:
3.4", 800×800). They plug face down onto 2×20, 2.54 mm headers on the main board. The firmware supports one, two or
three screens: each runs the same image and takes its role from a strap.

Pins the main board uses on each screen header (DigiDash2, `HDR_NETS` in `DigiDash2/generate/gen_sch.py`):

| Header pin | Left (J5) | Center (J1) | Right (J4) | What |
| --- | --- | --- | --- | --- |
| 1, 3 | +5 V | +5 V | +5 V | The screen's supply, from the board's 5 V |
| 5, 10, 13 (+ others) | GND | GND | GND | |
| 17 (GPIO4) | **1 kΩ to GND** | open | **1 kΩ to the screen's own 3V3 (pin 18)** | **Position strap**: left, center, right |
| 31 (GPIO34) | CAN TX | CAN TX | CAN TX | To that screen's transceiver TXD (10k pull-up) |
| 23 (GPIO36) | CAN RX | CAN RX | CAN RX | From its transceiver RXD |
| 15 | sensor serial RX | sensor serial RX | sensor serial RX | Optional serial fallback from the sensor card (10k / 20k divider from 5 V) |
| 34, 38 | cluster I2C SCL, SDA | SCL, SDA | SCL, SDA | Optional legacy theme sync; 4.7k pull-ups to 3.3 V. CAN does the same job |
| 28 (GPIO24) | — | illumination input | — | Center only: on the 4C, GPIO24 is USB D− |
| 27 | — | reverse input | — | Center only |
| 20, 22 | — | serial port (UART2) | — | Center only, to a general serial connector |
| 8, 11, 12, 14, 16, 24, 25, 30, 32, 35, 36, 37, 39 | — | camera video port (TVP5150 decoder) | — | Center only, optional |

The position strap decides the node number (center `0x01`, left `0x02`, right `0x03`) and what the screen shows. Two
screens on one bus must have different straps. A board with **one screen** leaves pin 17 open: it runs as the center
screen, which owns the cluster settings and can show every view.

Screen spacing, mounting holes and the panel centre relative to header pin 1 are in
[`DigiDash2/README.md`, Screen headers and screen holes](../../DigiDash2/README.md#screen-headers-and-screen-holes).

## Cable ports, trunk port and the debug tap

| Connector | Pinout | Use |
| --- | --- | --- |
| Cable port (JST PH 4-pin, S4B-PH-K) | 1 +5 V (0.5 A PTC), 2 CANH, 3 CANL, 4 GND (the DroneCAN / Pixhawk order) | Off-board CAN devices: the console, a sensor near the engine. Each is a stub: under 30 cm |
| Trunk port (Micro-Fit 3.0, 4-pin) | 1 CANH, 2 CANL (twisted pair), 3 switched 12 V (1.1 A PTC), 4 GND | Carries the bus itself to the rear of the car: switch the board's end terminator off, terminate at the far end |
| CAN tap (1×3 header) | CANH, CANL, GND | A USB-CAN adapter for watching and sending traffic (node `0x1F` is reserved for a service tool) |

A main board needs at least one cable port if you want the console. The trunk port and tap are optional.

## Mechanics of the slot

| Item | Value |
| --- | --- |
| Card | 80 × 60 mm, 1.6 mm thick; the 80 mm edge goes in the slot |
| Slot pitch | **25.4 mm** centre to centre (DigiDash2). Cards' parts sides all face the same way |
| Parts side height | up to 20 mm (the pitch, less the board, less the next card's 2.5 mm back side, less a margin). The end slot whose parts side faces off the row has no limit |
| Back side | up to 2.5 mm including solder tails |
| Card top edge | about 68.5 mm off the main board (the socket adds 8.5 mm); the card's connectors are there |
| Under the cards | keep main-board parts under about 7 mm inside each card's envelope |

If your enclosure has room for a different pitch, a wider pitch is always safe. A narrower one limits the height of the
cards' parts; check each card's README for its tallest part. Cards may be mounted at an angle or on a riser board,
as long as the 2×15 interface is kept.

## Minimum main boards

**Bench board.** [`BenchBoard/`](../../BenchBoard/) is a complete example of a small main board: 100 × 100 mm, two
layers, two slots, one screen header, 12 V in, a USB-CAN header. Start from it.

**The smallest useful board:**

1. A 12 V input with reverse protection and a 5 V regulator (enable from the ignition, OR'd with WAKE if any card needs it).
2. One screen header with a transceiver, pin 17 open (center), and 120 Ω at the transceiver.
3. One slot with its PTC on 5 V, ground, CANH / CANL, a slot ID resistor, and 120 Ω at the far end of the bus.
4. A cable port for the console, if wanted.

That runs a screen and the sensor card (or any other card) with no other changes to firmware.

**A board for a different car** typically changes: the outline and mounting, the number and spacing of screen
headers, the number of slots, the vehicle connectors (senders, speed, tach, lamps, camera), and the position of the
rear connector group. The slot pinout, slot IDs, bus termination rules and screen header pin use stay exactly as on
this page.

**Sensors in a different car.** The sensor card's inputs (senders, speed, tach) come out of its own top-edge
connector, not through the main board, and are calibrated from the menu
([`sensor_calibration.md`](../protocols/sensor_calibration.md)). If your car's senders differ, change or replace
the sensor card, not the main board. A new sensor card is just another card: publish readings on the SENSOR channel
and the screens and console show them.

## Bring-up checklist

Before fitting any card or screen:

- [ ] No short between +5 V, +3.3 V, 12 V rails and GND.
- [ ] CANH to CANL **about 60 Ω** with power off (two terminators). 120 Ω means one is missing; 40 Ω means three.
- [ ] Each slot's pin 11 to GND reads its own resistor value, and no two slots match.
- [ ] Socket pin numbering checked against a card header (pin 1 to pin 1, pin 30 to pin 30) by continuity, not by eye.
- [ ] Power on with no cards: +5 V and +3.3 V at every slot; switched 12 V follows the ignition; WAKE reads about 0 V.
- [ ] Pull WAKE to 3.3 V through a diode with the ignition off: the 5 V rail comes up.

Then with firmware:

- [ ] One screen: `can` on its serial console shows the controller running and no bus errors.
- [ ] Add a card: its heartbeat (`0x700 + node`) appears in `can` with the node number its slot implies.
- [ ] A USB-CAN adapter on the tap, or `python3 tools/can_relay.py --gateway center`, shows the traffic.

## Designing a card

The other half of the platform. A new card needs:

1. The **80 × 60 mm outline** and the 2×15 right-angle header on its bottom edge, numbered as above.
2. A **CAN controller and transceiver** (an ESP32 / ESP32-S3 / C3 / C6 with TWAI, an STM32 with FDCAN, or an MCP2515
   on SPI), with **no terminator**.
3. A **10k pull-up from pin 11 to its 3.3 V** into an ADC pin, so it can find its slot and node number.
4. A **programming header** reachable with the card in its slot (a right-angle header on a side edge), and a debug
   serial header.
5. Firmware that speaks the minimum of the channel protocol: a heartbeat and NET ANNOUNCE / QUERY / IDENTIFY. Then
   whichever channels it needs (SENSOR, ALERT, UI, AUDIO_CMD …).

Everything a card says on CAN, step by step, is in [`docs/protocols/can_guide.md`](../protocols/can_guide.md).
Examples of finished cards: [`SensorCard2/`](../../SensorCard2/), [`RadioCard/`](../../RadioCard/),
[`GPSCard/`](../../GPSCard/), [`AudioCarrier/`](../../AudioCarrier/), [`LTECard/`](../../LTECard/).
