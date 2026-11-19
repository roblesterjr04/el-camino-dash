# The CAN bus, explained

This is the guide to everything that crosses the El Camino Dash's CAN bus: how the bus is wired through the card
slots, how a node gets its number, how IDs and priorities are laid out, how messages longer than 8 bytes are framed,
and what a card says from power-on to normal running. It is written for someone designing a new card or a new main
board.

Two reference documents hold the exact byte layouts, and this guide links to them throughout:

- [`can_channels.md`](can_channels.md): the **channel protocol**, the public interface for cards and accessories.
- [`can_bus.md`](can_bus.md): the **cluster's own frames** (sensors to the screens, cluster settings, the audio
  bridge's media and phone frames, the console).

The code is `firmware/elc_can` ([its README](../../firmware/elc_can/README.md)); host tests in
`firmware/elc_can/test/host/`.

Contents:

1. [The big picture](#1-the-big-picture)
2. [Physical layer: from the card's pins to the bus](#2-physical-layer-from-the-cards-pins-to-the-bus)
3. [Nodes and node numbers](#3-nodes-and-node-numbers)
4. [The ID map and priority](#4-the-id-map-and-priority)
5. [Heartbeats](#5-heartbeats)
6. [Transfers: the channel framing](#6-transfers-the-channel-framing)
7. [Discovery: NET](#7-discovery-net)
8. [The channels, one by one](#8-the-channels-one-by-one)
9. [The cluster's own frames](#9-the-clusters-own-frames)
10. [A card's life on the bus](#10-a-cards-life-on-the-bus)
11. [Worked frames](#11-worked-frames)
12. [Bus budget](#bus-budget)
13. [Compatibility rules](#13-compatibility-rules)
14. [Tools for working on the bus](#14-tools-for-working-on-the-bus)
15. [Implementing a node](#15-implementing-a-node)

## 1. The big picture

```
   ┌───────────┐  ┌───────────┐  ┌───────────┐                       ┌──────────┐
   │ LEFT 0x02 │  │CENTER 0x01│  │RIGHT 0x03 │   round screens        │ console  │
   │ speedo    │  │ settings, │  │ fuel/oil/ │   (ESP32-P4)           │  0x13    │
   │           │  │ alerts,   │  │ temp      │                        │ ESP32-S3 │
   └─────┬─────┘  │ SYSTEM    │  └─────┬─────┘                        └────┬─────┘
         │        └─────┬─────┘        │                                   │ cable port
 120 Ω ══╪══════════════╪══════════════╪═══════╪═══════╪═══════╪═══════════╪══ … ══ 120 Ω
                                               │       │       │
                                         ┌─────┴──┐ ┌──┴─────┐ ┌┴────────┐
                                         │ slot 1 │ │ slot 2 │ │ slot n  │   cards (any MCU with CAN)
                                         │ audio  │ │ radio  │ │ sensor  │
                                         │ 0x14   │ │ 0x15   │ │ 0x04    │
                                         └────────┘ └────────┘ └─────────┘
```

- **One bus, everyone equal.** There is no master that polls. Every node sends what it has when it has it; every
  node hears everything and keeps what it wants.
- **Two families of messages share the bus:**
  - **Channels** (`can_channels.md`): a stable, documented, extensible interface. ALERT, SENSOR, UI, INPUT,
    AUDIO_CMD, SYSTEM, NET and so on. A new card uses these and needs no change to any other firmware.
  - **Cluster frames** (`can_bus.md`): the screens' and console's internal traffic (raw sensor frames, cluster
    settings, album art). Cards may listen, but these can change with the display firmware.
- **Services are found, not configured.** A card says what it is and what it produces and consumes (NET ANNOUNCE).
  The screens find the audio sink, the GPS source or the internet gateway from those announcements. No firmware
  holds a table of "the radio is node X".

## 2. Physical layer: from the card's pins to the bus

| Parameter | Value |
| --- | --- |
| Standard | Classic CAN 2.0A (11-bit IDs, up to 8 data bytes). No CAN FD: the ESP32 TWAI controllers are classic only |
| Bit rate | **500 kbit/s** (250 kbit/s as a whole-bus option for long cables: `ELC_CAN_BITRATE_KBPS=250` on every node) |
| Transceivers | One per node: MCP2562 or TJA1051T/3, VDD 5 V, VIO 3.3 V, standby tied low |
| Termination | **120 Ω at the two physical ends of the bus, on the main board only.** 60 Ω between CANH and CANL with power off |
| Topology | One linear bus on the main board; screens, slots and cable ports are short stubs |
| Stub length | Under about 30 cm at 500 kbit/s |
| ESD | A NUP2105L on the main board's bus (and at the trunk port); cards with long cables fit their own |
| Frame sending | **Single-shot** (no automatic hardware retransmit); messages repeat instead (see [§13](#13-compatibility-rules)) |
| Bus-off | Recovered automatically, retry every second |

### Through the card slot

A card meets the bus on two pins of its 2×15 slot header. The main board's slot pinout puts ground on both sides of
the pair:

```
         odd   even
 1/2    +5V    +5V
 3/4    GND    GND      ← ground beside the pair
 5/6   CANH   CANL      ← the bus
 7/8    GND    SW12V    ← ground beside the pair
 9/10   3V3    BATT12V
11/12  SLOT ID WAKE     ← slot ID gives the node number
 …
```

On the card:

```
 slot pin 5 (CANH) ──────────── CANH ┐
 slot pin 6 (CANL) ──────────── CANL ├─ transceiver (MCP2562 / TJA1051T/3) ── TXD / RXD ── MCU's CAN controller
 slot pin 1/2 (+5V) ─── VDD (5 V)    │                                                    (TWAI, FDCAN, or MCP2515)
 card 3.3 V ─────────── VIO (3.3 V)  │
 GND ────────────────── STBY, GND    ┘
                         no 120 Ω on the card, ever
```

- **Never terminate on a card.** The main board's two 120 Ω make the bus correct with any set of cards plugged in or
  pulled out. A card with its own terminator lowers the bus impedance and causes errors for everyone.
- **Hold TXD recessive while the MCU boots.** A pull-up on TXD (10k to 3.3 V) keeps a booting or blank MCU from
  jamming the bus. The main board does this for the screens.
- **Keep the stub short on the card too:** the transceiver near the slot header, CANH / CANL side by side.
- On the main board the screens' transceivers sit next to their headers (header pin 31 = TX, pin 23 = RX). The full
  main-board side is in [`docs/hardware/main_board_guide.md`](../hardware/main_board_guide.md#the-can-bus-on-the-main-board).

### Without a bus at all

On the bench, boards can share CAN over USB instead: every `elc_can` board carries its CAN traffic over its USB
serial port when `tools/can_relay.py` asks for it, and the relay copies frames between them. Same firmware, no
transceivers. See [`usb_link.md`](usb_link.md) and [§14](#14-tools-for-working-on-the-bus).

## 3. Nodes and node numbers

Every node has a **5-bit node number** (`0x01`–`0x1F`). It appears in the CAN ID of everything the node sends, so
two nodes can never send the same ID (CAN forbids it: two senders of one ID corrupt each other's frames).

| Node | Who | How it gets its number |
| --- | --- | --- |
| `0x00` | nobody | as a destination: "every node" |
| `0x01` | center screen | screen position strap (header pin 17 open) |
| `0x02` | left screen (speedometer) | strap: pin 17 to GND |
| `0x03` | right screen | strap: pin 17 to the screen's 3.3 V |
| `0x04` | sensor card | **fixed**, in any slot (there is only ever one) |
| `0x05`–`0x0F` | reserved for the dash's own nodes | |
| `0x10`–`0x12` | rev 1 carriers (header positions on `DigiDash/`) | straps on the carrier |
| `0x13` | console | fixed in its firmware |
| **`0x14`–`0x19`** | **slot cards, by slot** | **`0x14 + (slot − 1)`, from the slot ID pin** |
| `0x1A`–`0x1B` | other cable-port devices | set in their own configuration |
| `0x1C`–`0x1E` | accessories without a private range (channels only) | |
| `0x1F` | a service tool (a laptop on the CAN tap) | |

### How a card finds its slot

Pin 11 of each slot has a resistor to ground on the main board, a different value per slot. The card puts a 10k
pull-up to its own 3.3 V on that pin and reads it on an ADC:

| Slot | Resistor | Volts | Node |
| --- | --- | --- | --- |
| 1 | 1k | 0.30 | `0x14` |
| 2 | 3.3k | 0.82 | `0x15` |
| 3 | 6.8k | 1.34 | `0x16` |
| 4 | 12k | 1.80 | `0x17` |
| 5 | 22k | 2.27 | `0x18` |
| 6 | 47k | 2.72 | `0x19` |
| — | open | ~3.3 | no slot |

```c
// firmware/elc_can/include/elc_can_ids.h
uint8_t slot = elc_can_card_slot_from_mv(mv);    // 1..6, or 0 = no valid slot
uint8_t node = 0x14 + (slot - 1);
```

A card that reads **no valid slot** (on the bench, without a main board) does not guess: it stays off the bus and
says why on its debug serial, unless its console is given a node for the bench. (The audio card is the one
exception: it takes `0x14` so the shipped image runs on a bare DevKit.)

**Why by slot and not by card type?** No configuration, ever. Two identical cards in one car never clash, and moving
a card to another slot just gives it a new number. Nobody needs to know a card's number in advance, because services
are found by discovery ([§7](#7-discovery-net)).

## 4. The ID map and priority

In CAN, **a lower ID wins arbitration**: when two nodes start sending together, the one with the lower ID carries on
and the other waits. The ID map uses that: urgent, rare traffic low; bulky traffic high.

Each channel is a block of 32 IDs, and **the ID is `channel base + node`**:

```
 11-bit ID:   c c c c c c n n n n n
              └─ channel ┘└─ node ┘       channel = ID >> 5,  node = ID & 0x1F
```

| IDs | What | Kind |
| --- | --- | --- |
| `0x000`–`0x01F` | reserved, kept free (highest priority) | |
| `0x020 + node` | **ALERT** | channel |
| `0x040 + node` | **INPUT** | channel |
| `0x060 + node` | **SYSTEM** | channel |
| `0x080`–`0x0FF` | reserved | |
| `0x100`–`0x104` | raw sensor frames for the screens | cluster frame |
| `0x120 + node` | **SENSOR** | channel |
| `0x140`–`0x1FF` | reserved | |
| `0x210`–`0x226` | cluster state, settings, menu, time, GPS | cluster frames |
| `0x240 + node` | **AUDIO_CMD** | channel |
| `0x260 + node` | **UI** | channel |
| `0x300 + node` | **AUDIO_STREAM** (raw) | channel |
| `0x320 + node` / `0x340 + node` | **BULK** control / data (raw) | channel |
| `0x360 + node` | **ONLINE** | channel |
| `0x3C0 + node` | **NET** | channel |
| `0x3E0 + node` | **LOG** | channel |
| `0x400 + 0x40·(node − 0x10)` … +`0x3F` | node-private range (64 IDs per accessory node) | private |
| `0x700 + node` | heartbeat | |
| `0x720`–`0x7FF` | reserved; avoid `0x7DF`, `0x7E0`–`0x7EF` (OBD-II) | |

Examples: the card in slot 3 (node `0x16`) sends SENSOR on `0x136`, UI on `0x276`, NET on `0x3D6`, its heartbeat on
`0x716`, and owns the private range `0x580`–`0x5BF`.

**Rule:** nothing below `0x100` may be sent periodically faster than 1 Hz, apart from bursts caused by a change. That
keeps alerts and input fast no matter what else is happening.

## 5. Heartbeats

Every node sends a heartbeat **once a second** on `0x700 + node`. It is the liveness signal everyone uses: the
center lists what is on the bus from it, a card that loses the audio card's heartbeat stops driving the I2S rail, and
an alert source that stops is marked "source lost".

| Byte | Field |
| --- | --- |
| 0 | node type (0 display, 1 audio bridge, 2 console, 3 sensor node, 4 input device, 5 audio sink, 6 output / relay, 7 network gateway, 8 voice assistant, 9 radio / media source, `0x7F` service tool) |
| 1–2 | firmware version, u16 LE |
| 3–6 | uptime in seconds, u32 LE |
| 7 | flags: **bit0 = this node speaks the channel protocol** |

```
0x716  [8]  03 01 00 10 0E 00 00 01      node 0x16: type 3 (sensor node), fw 1, up 3600 s, speaks channels
```

## 6. Transfers: the channel framing

A channel message is a **transfer**: a payload of 1–256 bytes, in one frame or several. The framing borrows the
DroneCAN (UAVCAN v0) tail byte: one byte at the end of every frame, no handshake, works for broadcast. (Only the
framing is borrowed; the IDs are this project's, so it is not DroneCAN on the wire.)

### The tail byte

```
 bit   7     6     5       4 3 2 1 0
      SOT   EOT  toggle   transfer ID (0–31)
```

- **SOT**: first frame of the transfer. **EOT**: last frame.
- **toggle**: 0 in the first frame, then alternates. Catches a lost or repeated frame.
- **transfer ID**: the sender counts it up per transfer on that channel, wrapping at 32. Catches duplicates.

### Single frame (payload up to 7 bytes)

```
 [ payload (1–7 bytes) ][ tail: SOT=1 EOT=1 toggle=0 TID ]      DLC = payload + 1
```

### Multi-frame (payload 8–256 bytes)

1. Compute the **CRC-16/CCITT-FALSE** of the payload (poly `0x1021`, init `0xFFFF`, no reflection, no final XOR;
   check: `"123456789"` → `0x29B1`).
2. Put the CRC in front of the payload, **low byte first**.
3. Cut CRC + payload into 7-byte pieces; each piece gets a tail byte. Only the last frame may be short.

```
 frame 1:  [CRC lo][CRC hi][payload 0..4 ][tail SOT=1 EOT=0 t=0 TID]
 frame 2:  [payload 5..11                 ][tail SOT=0 EOT=0 t=1 TID]
 frame 3:  [payload 12..18                ][tail SOT=0 EOT=0 t=0 TID]
 last:     [payload … (1–7 bytes)         ][tail SOT=0 EOT=1 t=? TID]
```

### Receiving

Keep one reassembly buffer per CAN ID you care about (that is, per channel and sender):

- SOT starts a new transfer and drops any unfinished one.
- Append a frame only if its transfer ID matches and its toggle is the one expected; otherwise drop the transfer.
- More than **100 ms** between frames of one transfer drops it.
- At EOT check the CRC; a bad CRC or more than 256 bytes drops it.
- A transfer with the same transfer ID as the last one completed from that sender on that channel, within 1 s, is a
  duplicate: ignore it.

### Sending

- Finish one transfer on a channel before starting the next on the same channel. Different channels may interleave.
- Transfer frames are sent with a bounded retry (up to 4 attempts within 20 ms), in order. The cluster's periodic
  frames stay single-shot.
- **Raw** channels (AUDIO_STREAM, BULK_DATA) have no tail byte; they are paced streams with their own sequence numbers.

### The payload

```
 byte 0      opcode       0x00–0x7F broadcast, 0x80–0xFF addressed
 byte 1      destination  (addressed opcodes only; 0x00 = every node)
 byte 2…     fields       little-endian integers; UTF-8 text, length from the transfer length
```

The sender is never in the payload: it is in the CAN ID. Unknown opcodes and extra trailing bytes are ignored.

## 7. Discovery: NET

NET (`0x3C0 + node`) is what every channel-speaking node must implement. It answers "who is on the bus and what can
they do?"

| Op | Name | Direction | Purpose |
| --- | --- | --- | --- |
| `0x01` | ANNOUNCE | broadcast | I am: protocol version, node type, product, firmware, unique ID, description generation, **produces** and **consumes** bitmaps, flags (incl. my slot), name |
| `0x81` | QUERY | addressed | Please (re)send your ANNOUNCE and / or your sensor, alert, UI descriptions |
| `0x82` | IDENTIFY | addressed | Blink an LED or beep for N seconds so a person can find you |
| `0x83` | RESTART | addressed | Restart (from the center or a service tool, with a magic byte) |
| `0x84` | RAIL | addressed | Ask for / grant / release the I2S rail's return line |
| `0x11` | GATEWAY STATUS | broadcast | An internet gateway's link state |
| `0x8B` | GATEWAY CMD | addressed | A command to the gateway |

**produces / consumes** are bitmaps by channel number (bit 1 ALERT, bit 2 INPUT, bit 3 SYSTEM, bit 9 SENSOR, bit 18
AUDIO_CMD, bit 19 UI, …). The node that **consumes AUDIO_CMD** is the audio sink; the node that **consumes ALERT and
SENSOR descriptions** is the center. That is how services are found.

The startup dance:

```
 card boots ─► heartbeat (flags bit0 = 1) ─► NET ANNOUNCE ─► its descriptions (SENSOR DEFINE, ALERT DEFINE, UI ITEM)
 center boots ─► NET QUERY to 0x00, all bits ─► every node answers after (node × 8) ms, so they don't all talk at once
 a node heartbeats but never announced ─► the center QUERYs it
```

The **description generation** byte changes whenever any of a node's descriptions change, so a receiver may cache
descriptions by (unique ID, generation). The **unique ID** (from the MAC or chip ID) also catches two cards with one
node number: two ANNOUNCEs, one number, two IDs.

## 8. The channels, one by one

Each line below is a summary; the byte layouts are in [`can_channels.md`](can_channels.md).

| Channel | Who sends | What for | Key rule |
| --- | --- | --- | --- |
| [ALERT](can_channels.md#alert-0x020--node) `0x020` | any node; center sends SUMMARY and ACK | Define up to 32 alerts per node (severity, icon, sound, text); STATE says which are active | STATE **at 1 Hz always**; 3 s of silence = "source lost". The center runs the one alert list and plays the chimes |
| [INPUT](can_channels.md#input-0x040--node) `0x040` | button / knob nodes | KEY (click, long press, repeat) and ENCODER (cumulative position) | Positions are cumulative so a lost frame loses no clicks. Every consumer acts on its own keys; nobody routes |
| [SYSTEM](can_channels.md#system-0x060--node) `0x060` | center | TIME, VEHICLE (ignition, running, lights, reverse, speed, backlight %), PREFS (units, theme), POWER (ignition off: save now) | Cards never need the sensor maths; they read results |
| [SENSOR](can_channels.md#sensor-0x120--node) `0x120` | any node; the center publishes the car's core readings as node `0x01` | DEFINE (quantity, exponent, period, display range, thresholds, name) and VALUE (s32 + status) | value = raw × 10^exponent in the canonical unit (°C, kPa, km/h …); the screens convert for display. Optional: let the center raise alerts from the thresholds |
| [AUDIO_CMD](can_channels.md#audio_cmd-0x240--node) `0x240` | any node to the audio sink; sink sends STATUS / DONE | PLAY / STOP stored sounds, VOLUME, MEDIA (play, next, tune, presets), SOURCE; NOW PLAYING from the active source | Music never crosses CAN. Sounds are stored on the sink and triggered by number |
| [UI](can_channels.md#ui-0x260--node) `0x260` | the card; the menu screen sends SET / VIEW | A tree of items (group, readout, toggle, number, choice, action, text) shown in Menu → Accessories | No pixels on the bus. The card applies SET, saves it and echoes VALUE + RESULT. Live values only while VIEWed |
| [AUDIO_STREAM](can_channels.md#audio_stream-0x300--node-raw) `0x300` | one granted node | IMA-ADPCM voice, 8 or 16 kHz | One stream at a time, granted by the sink; 16 % / 31 % of the bus |
| [BULK](can_channels.md#bulk-0x320--node-control-0x340--node-data) `0x320` / `0x340` | owner ↔ receiver | Files: sound clips to the sink, later firmware | Receiver-driven credit (WANT), CRC-32, paced to a third of the bus |
| [ONLINE](can_channels.md#online-0x360--node) `0x360` | the internet gateway | PLACE, WEATHER, TIMEZONE, MESSAGE, STATUS | The gateway does the lookups; only answers cross the bus |
| [NET](can_channels.md#net-0x3c0--node) `0x3C0` | everyone | Discovery, identify, restart, I2S rail, gateway | Mandatory |
| [LOG](can_channels.md#log-0x3e0--node) `0x3E0` | any node | Text lines to the center's SD log and console | At most 10 lines/s per node |

### The I2S rail, over CAN

The slots share a digital audio bus (I2S: BCK, WS, MIC, RET, MCLK). The audio card is its clock master. **RET**
(audio back to the audio card) has one owner at a time, granted over CAN:

```
 card ──► NET RAIL to the audio card, state 0 (request)
 audio card ──► NET RAIL, state 1 (granted) or 2 (denied)      only after "granted" may the card enable its RET driver
 card ──► NET RAIL, state 2 (release) when done
 audio card ──► state 2 to the holder to withdraw it (a voice card outranks everyone; the radio ranks lowest)
 holder loses the audio card's heartbeat ──► stops driving RET at once
 audio card loses the holder's heartbeat for 3 s ──► frees RET
```

This is how the radio card plays through the audio card's DACs, and how a voice card can speak over the music.

## 9. The cluster's own frames

These are in [`can_bus.md`](can_bus.md). Cards may read them; anything a card depends on should be (or will be) on a
channel instead.

| ID | From | What |
| --- | --- | --- |
| `0x100`–`0x104` | sensor card (and the center re-sends) | Raw sensor frames for the gauge faces: wheel and engine rpm, sender ADC values, flags and faults, odometer, economy. 20 Hz |
| `0x210` | center | Cluster state: theme, center view, interface screen, media screen, brightness, flags |
| `0x211` | a screen / the console | Album art request |
| `0x212` | anyone | Setting request (the center applies it and answers with `0x210`) |
| `0x213` | center | Center status (GPS fix, demo, in reverse …) |
| `0x215` | console, screens | Phone command to the audio bridge (answer, end, pair, connect …) |
| `0x216`–`0x219` | screens | Remote menu input and the menu's state and texts |
| `0x220` | center | Time (UTC, local offset, GPS fix) |
| `0x222`–`0x226` | GPS card | Position, motion, quality, time, status |
| `0x400`+ | audio bridge (its node range) | Transport, now-playing text, album art, phone status, phone text, pairing |

The audio bridge's album art is a good example of moving a few kilobytes over classic CAN: the receiver asks
(`0x211`), the bridge streams 5-byte chunks paced to a third of the bus, and the receiver re-asks for missing chunks
after 300 ms of quiet. BULK generalises that for everything else.

## 10. A card's life on the bus

What a well-behaved card does, from power-on, using the transmission-temperature card in `can_channels.md` as the
example (node `0x14` in slot 1).

```
 t = 0        power from slot pins 1/2. TXD held recessive by its pull-up while the MCU starts
 t ≈ 10 ms    read slot pin 11 → 0.30 V → slot 1 → node 0x14. No valid slot → stay off the bus, log why
 t ≈ 20 ms    start the CAN controller at 500 kbit/s, single-shot, accept all IDs
              heartbeat on 0x714 (type 3, fw, uptime 0, flags bit0)
              NET ANNOUNCE on 0x3D4 (multi-frame): type 3, product, unique ID, produces SENSOR+ALERT+UI, consumes SYSTEM, "Trans temp"
              SENSOR DEFINE sensor 0 on 0x134 (multi-frame): temperature, exponent −1, 200 ms, ranges, thresholds
              UI ITEM 0 on 0x274 (multi-frame): number "Offset", °C
 every 1 s    heartbeat
 every 200 ms SENSOR VALUE sensor 0 on 0x134 (one frame)
 on QUERY     re-send ANNOUNCE and the descriptions asked for, after (node × 8) ms
 on SET       apply, save, answer UI VALUE + RESULT
 on VIEW      stream live UI values (≤ 5 Hz) while the last VIEW is under 3 s old
 on IDENTIFY  blink its LED
 on SYSTEM POWER (ignition off)   save state; hold WAKE (slot pin 12) high if it needs more time, release when done
```

Nothing in the screens, center or console changes for this card: its reading appears as a readout the screens can
show, the center logs it and raises "Trans temp high" from its thresholds, and its Offset setting appears under
Menu → Accessories → Trans temp on the round screens and on the console.

**The minimum** a node must implement: heartbeat (flags bit0) and NET ANNOUNCE / QUERY / IDENTIFY. Then the channels
it announces. Recommended: LOG, and SYSTEM POWER.

## 11. Worked frames

### SENSOR VALUE, single frame

Node `0x14` reports 104.3 °C (exponent −1, so 1043), status ok, transfer ID 7:

```
ID 0x134 (0x120 + 0x14)   DLC 8
  02          opcode VALUE
  00          sensor 0
  13 04 00 00 value 1043 (s32 LE)
  00          status ok
  C7          tail: SOT 1, EOT 1, toggle 0, TID 7   (0x80 | 0x40 | 0x07)
```

### ALERT DEFINE, multi-frame

Node `0x14` defines alert 0: caution (2), needs acknowledgement (flags 1), icon temperature (1), default sound
(`0x0000`), text "Trans fluid hot". Payload 22 bytes: `01 00 02 01 01 00 00` + 15 bytes of text. CRC-16 = `0xC15F`.
Transfer ID 3:

```
ID 0x034  [8]  5F C1 01 00 02 01 01 83     CRC lo, CRC hi, op, alert, severity, flags, icon | tail SOT, t=0, TID 3
ID 0x034  [8]  00 00 54 72 61 6E 73 23     sound u16, "Trans"                               | tail t=1
ID 0x034  [8]  20 66 6C 75 69 64 20 03     " fluid "                                        | tail t=0
ID 0x034  [4]  68 6F 74 63                 "hot"                                            | tail EOT, t=1
```

### ALERT STATE, every second

```
ID 0x034  [7]  02 01 00 00 00 01 CC        op STATE, active = 0x00000001 (alert 0), generation 1 | tail SOT|EOT, TID 12
```

### Heartbeat

```
ID 0x714  [8]  03 01 00 10 0E 00 00 01     type 3, fw 1, uptime 3600 s, flags bit0
```

## Bus budget

At 500 kbit/s a full 8-byte frame takes about 270 µs with worst-case bit stuffing: about **3,700 frames/s, ~25 KB/s**
of payload if nothing else ran.

| Traffic | Share of the bus |
| --- | --- |
| Cluster frames, heartbeats, audio transport (today) | ~5 % |
| Core sensors on SENSOR (center) | ~2 % |
| SYSTEM, ALERT, INPUT, UI, NET, LOG, ONLINE | < 1 % |
| Accessory sensors (budget: 20 values at 10 Hz) | ~6 % |
| BULK and album art together (paced, only while transferring) | ≤ 34 % |
| AUDIO_STREAM, one at a time (8 / 16 kHz) | 16 % / 31 % |

Design ceiling: about 70 %. Because bulk and streams have the highest IDs, they never delay alerts, input, system or
sensor traffic.

What does **not** fit: music. CD-quality audio is 176 KB/s. Music stays on the audio card (phone → A2DP → DAC) or on
the I2S rail between cards; CAN carries commands and now-playing text only.

## 13. Compatibility rules

These rules let old cards keep working with new screens and the other way round.

1. **Ignore what you do not know.** Unknown IDs, opcodes, trailing bytes and registry codes are ignored.
2. **Fields are only added at the end.** An opcode's meaning never changes. Registries (products, quantities, keys,
   icons, sounds) are append-only.
3. **State, not events.** Wherever a lost frame would matter, the message carries repeated state (an alert's active
   bit, an encoder's cumulative position, a setting echoed back). Frames are single-shot; the next repeat repairs a
   loss.
4. **Describe once, update small.** Names, units and labels go in multi-frame descriptions at boot and on QUERY;
   values are single frames.
5. **Public versus internal.** Channels are the stable interface. Cluster frames (`0x100`–`0x226`) are internal and
   may change with the display firmware.
6. **The sender is in the ID.** Never reuse another node's ID; never put the sender in the payload.

## 14. Tools for working on the bus

| Tool | What |
| --- | --- |
| `can` on any screen's or the console's serial console | counters, bus errors, the nodes heard and their heartbeats |
| `python3 tools/can_relay.py` | ties every USB-connected board into one bus over USB; simulates the drive, the clock and every card not yet built |
| `python3 tools/can_relay.py --frames` | the same, with a frame log (`:filter 0x215` to narrow it) |
| `python3 tools/can_relay.py --gateway center` | in the car: a laptop on the center's USB joins the real bus |
| a USB-CAN adapter on the main board's CAN tap | a service tool on the real bus (node `0x1F` is reserved for it) |
| `tools/cards_sim.py`, `tools/radio_sim.py`, `tools/chan_sim.py` | Python implementations of the channel protocol and of each planned card: a readable reference for a new card's behaviour |
| `firmware/elc_can/test/host/run_tests.sh` | the C++ transport and codecs, tested on the host |

See [`tools/README.md`](../../tools/README.md).

## 15. Implementing a node

**With `elc_can` (ESP32 family, Arduino framework).** The library has the TWAI driver, heartbeat, bus-off recovery,
the USB link, the channel transport, NET and the message codecs. The loop contract:

```cpp
#include <elc_can.h>

static ElcCan s_can;
static ElcChannels s_chan;

void setup() {
  elc_usb_begin();                                    // Serial with the USB CAN link
  uint8_t slot = elc_can_read_card_slot(SLOT_ID_ADC_PIN);  // slot pin 11 via the card's 10k pull-up; 0 = none
  if (slot == 0) { /* no valid slot: stay off the bus, log why */ return; }
  uint8_t node = 0x14 + (slot - 1);
  s_can.onFrame(on_frame, nullptr);
  s_can.begin(ELC_CAN_TX_PIN, ELC_CAN_RX_PIN, node, ElcCanNodeType::Unknown /* or your type */);
  ElcNetAnnounce me;                                  // name, type, product, produces / consumes
  s_can.attachChannels(&s_chan, me);                  // heartbeat flag bit0
  s_chan.onTransfer(on_transfer, nullptr);
}

void loop() { s_can.poll(); }
```

See [`firmware/elc_can/README.md`](../../firmware/elc_can/README.md) for the API, build flags and how to add a
message.

**On another MCU.** The transport, NET and codecs (`elc_chan.cpp`, `elc_chan_msg.cpp`, `elc_can_codec.cpp`) are
plain C++ with no Arduino dependency: the sensor cards build them on an ATmega2560 (with an MCP2515) and an
STM32G0B1 (with its FDCAN) and supply their own CAN driver. Reuse those files, or write your own from this guide
and `can_channels.md`; the Python simulators in `tools/` are a second reference.

**Checklist for a new card's firmware:**

- [ ] Reads its slot from pin 11 and computes its node number; stays off the bus with no valid slot.
- [ ] 500 kbit/s, 11-bit IDs, accepts all IDs, ignores unknown ones.
- [ ] Heartbeat at 1 Hz on `0x700 + node`, flags bit0 set.
- [ ] NET ANNOUNCE at boot and on QUERY (after node × 8 ms); IDENTIFY does something visible.
- [ ] A product number from the registry (ask for one in an issue, or use `0x8000`+ for a private product).
- [ ] Descriptions (SENSOR DEFINE, ALERT DEFINE, UI ITEM) at boot and on QUERY; generation bumped when they change.
- [ ] Repeats state as each channel requires (ALERT STATE at 1 Hz, SENSOR VALUE at its period).
- [ ] Never drives the I2S rail's RET without a grant; releases it when the audio card's heartbeat stops.
- [ ] Never terminates the bus; holds TXD recessive at boot.
