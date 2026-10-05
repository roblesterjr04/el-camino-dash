# CAN channels: the accessory protocol

**Status: specification, version 1 (draft), partly implemented.** The transport, NET, SYSTEM, SENSOR, LOG and UI
are in use: `firmware/elc_can` (`elc_chan`, and `accessory_can` for accessory UI pages) runs on the three round
screens, the console and the sensor card. The center sends SYSTEM and the core readings on SENSOR and logs LOG lines;
the sensor card publishes SENSOR readings, ALERTs and a UI page; the round screens' menu (Accessories) and the
console's menu (Accessories, Radio) show every accessory's UI page and send SET / VIEW; the console sends
AUDIO_CMD (SOURCE, MEDIA, VOLUME) and reads NOW PLAYING. The center runs the ALERT manager (`elc_alerts`,
`center_alerts.cpp`): it collects every node's alerts, raises its own, plays the chimes and sends ACK and SUMMARY.
**Not implemented yet:** the alert banner and list on the menu screen (the bench acknowledges with the center's
`alerts ack`), the center raising alerts from other nodes' SENSOR thresholds (flag bit2), the accessory settings backup and restore on the center's SD
card, INPUT (no node sends or acts on it), BULK and AUDIO_STREAM users, and most of the AUDIO_CMD sink: the audio
card's firmware (the audio bridge, `arduino/sketches/esp32_audio_bridge`) plays the alert chimes (PLAY / STOP of
sounds 2-4, DONE) and has a UI page (Chime volume), but does not announce as node type 5 or do STATUS, SOURCE, VOLUME or MEDIA yet, and the radio card
has no firmware (`tools/cards_sim.py` and `tools/radio_sim.py` simulate them for the bench).
The bus itself, the legacy messages
and the node numbers are in [`can_bus.md`](can_bus.md); the ports carriers plug into are in
[`../hardware/accessory_ports.md`](../hardware/accessory_ports.md).

A carrier board should be able to use what the dash already does (show an alert, put a reading on a screen, add a
page to the menu, play a sound, take the knobs and keys) without the dash firmware knowing the carrier exists. This
document defines that interface as a set of **channels**, one per dash service, on the existing 500 kbit/s bus.

| Channel | What an accessory gets | Served by |
| --- | --- | --- |
| ALERT | Raise and clear alerts; the dash shows them, chimes and tracks acknowledgement | center (alert manager), the menu screen |
| SENSOR | Publish readings with a unit; the dash can show, log and alarm on them. The center publishes the car's own readings here too | center (logger, thresholds), screens, console |
| UI | A page of items (readouts, toggles, numbers, choices, buttons) in the dash menu; settings backed up on the center's SD card | whichever screen shows the menu; center (backup) |
| INPUT | Send keys and knobs that drive the menu and media, or listen to them | menu screen, center, audio sink |
| AUDIO_CMD | Play and stop sounds from the sink's library; volume, media transport | audio sink |
| AUDIO_STREAM | Stream a voice-quality sound (short, one at a time) | audio sink |
| SYSTEM | Time, ignition, lights, reverse, speed, units, dimming, power-down notice | center |
| BULK | Move files: sound clips to the sink, later firmware images | whoever owns the object |
| NET, LOG | Discovery, capabilities, identify, text logs to the center's SD card | everyone / center |

## Is this realistic?

Yes, with one limit. Classic CAN carries 8 data bytes per frame and at 500 kbit/s a full 8-byte frame takes about
270 µs with worst-case bit stuffing: about **3,700 frames/s, ~25 KB/s of payload** if nothing else ran. Alerts,
sensors, menu items, keys and sound commands are a few frames each and cost well under 1 % of the bus.

The limit is **audio signal**. Music (CD quality is 176 KB/s) cannot cross this bus and never will: it stays on the
audio carrier, from the phone's A2DP stream straight to its DAC. What fits is **voice-quality** audio: 4-bit ADPCM at
8 kHz uses about 16 % of the bus, at 16 kHz about 31 %. So sounds an accessory needs are **stored on the audio sink
and triggered by command** (AUDIO_CMD), uploaded once over BULK if the sink does not already have them. Streaming is
the last resort, one stream at a time.

Moving to CAN FD would lift the limit (64-byte frames, faster data phase), but the ESP32 TWAI controllers and the
MCP2562 are classic-CAN only. Version 1 stays on classic CAN.

## Design rules

1. **Public vs internal.** The channels are the public, stable interface. The legacy IDs `0x100`–`0x214` are the
   cluster's internal traffic: accessories may read them, but they can change with the display firmware. Anything an
   accessory depends on is (or will be) on a channel.
2. **The source node is in the CAN ID.** Every channel ID is `channel base + node`. Two nodes can therefore never
   send the same ID (which CAN forbids), receivers know the sender without spending a byte, and reassembly is keyed
   by ID.
3. **Priority follows the channel.** Lower ID wins arbitration. Rare, urgent channels sit below the sensor data;
   bulky ones sit above it. **Nothing below `0x100` may be sent periodically faster than 1 Hz**, apart from bursts
   caused by a change.
4. **State, not just events.** Wherever loss would matter, the message carries state that is repeated (an alert's
   active bit, an encoder's cumulative position, a setting's value echoed back). A lost frame is fixed by the next
   one.
5. **Describe once, update small.** Descriptions (an alert's text, a sensor's name and unit, a menu item's label) are
   multi-frame and sent at boot and on request. Values are single frames.
6. **Ignore what you do not know.** Unknown opcodes, extra trailing bytes and unknown registry codes are ignored.
   Fields are only ever added at the end; an opcode's meaning never changes. That is how v1 devices keep working with
   v2 dashes.

## ID map

The 11-bit ID space, lowest (highest priority) first. Channel number = ID >> 5; each channel is 32 IDs,
`base + node`.

| IDs | Ch | Use | Framing |
| --- | --- | --- | --- |
| `0x000`–`0x01F` | 0 | reserved (highest priority, kept free) | |
| `0x020`–`0x03F` | 1 | **ALERT** | transfer |
| `0x040`–`0x05F` | 2 | **INPUT** | transfer |
| `0x060`–`0x07F` | 3 | **SYSTEM** | transfer |
| `0x080`–`0x0FF` | 4–7 | reserved | |
| `0x100`–`0x11F` | 8 | legacy sensor frames (`0x100`–`0x104`, `can_bus.md`) | legacy |
| `0x120`–`0x13F` | 9 | **SENSOR** | transfer |
| `0x140`–`0x1FF` | 10–15 | reserved | |
| `0x200`–`0x23F` | 16–17 | legacy cluster state, requests, center status, GPS reserve | legacy |
| `0x240`–`0x25F` | 18 | **AUDIO_CMD** | transfer |
| `0x260`–`0x27F` | 19 | **UI** | transfer |
| `0x280`–`0x2FF` | 20–23 | reserved | |
| `0x300`–`0x31F` | 24 | **AUDIO_STREAM** | raw |
| `0x320`–`0x33F` | 25 | **BULK** (control) | transfer |
| `0x340`–`0x35F` | 26 | **BULK_DATA** | raw |
| `0x360`–`0x37F` | 27 | **ONLINE** (internet results from a gateway) | transfer |
| `0x380`–`0x3BF` | 28–29 | reserved | |
| `0x3C0`–`0x3DF` | 30 | **NET** | transfer |
| `0x3E0`–`0x3FF` | 31 | **LOG** | transfer |
| `0x400`–`0x6FF` | 32–55 | node-private ranges (`0x400 + 0x40·(node − 0x10)`): messages only that accessory's partners understand | private |
| `0x700`–`0x71F` | 56 | heartbeats (already `base + node`) | legacy |
| `0x720`–`0x7FF` | 57–63 | reserved. Avoid `0x7DF` / `0x7E0`–`0x7EF` (OBD-II diagnostics) | |

The node-private ranges stay: the audio bridge's now-playing text and album art (`0x400`–`0x403`) keep working
unchanged. New accessories should use channels for anything the dash acts on, and their private range only for
traffic between themselves and their own partner boards.

### Node numbers

Five bits, `0x00`–`0x1F`, as in `can_bus.md`:

| Node | Who |
| --- | --- |
| `0x00` | never a source. As a destination: every node |
| `0x01`–`0x03` | center, left, right screens |
| `0x04`–`0x0F` | the dash's own nodes: `0x04` the sensor pack (the sensor card, `SensorCard/`; a fixed number in any slot). `0x05`–`0x0F` reserved |
| `0x10`–`0x1B` | accessories with a private range: `0x10`–`0x12` rev 1 carriers, `0x13` the console, **`0x14`–`0x19` the slot cards by slot (`0x14` + slot − 1)**, `0x1A`–`0x1B` other cable-port devices (`docs/hardware/accessory_ports.md`, Node numbers) |
| `0x1C`–`0x1E` | accessories without a private range (channels only) |
| `0x1F` | a service tool (laptop on the J16 tap with a USB-CAN adapter) |

The heartbeat (`0x700 + node`) stays the liveness signal. **Heartbeat flags bit0 = the node speaks this protocol**
(version in its ANNOUNCE). The flags byte is 0 in today's firmware, so old nodes read as "not speaking it".

## Transport

Channel messages are **transfers**: a payload of 1–256 bytes, carried in one frame or several. The framing is the
DroneCAN (UAVCAN v0) tail byte, an established and documented scheme with one byte of overhead, which works for
broadcast (unlike ISO-TP, which needs a flow-control reply). Only the framing is borrowed: the IDs are ours, so this
is not DroneCAN on the wire.

### Tail byte

The last data byte of every transfer frame:

| Bit | Name | Meaning |
| --- | --- | --- |
| 7 | SOT | first frame of the transfer |
| 6 | EOT | last frame of the transfer |
| 5 | toggle | 0 in the first frame, then alternates |
| 4–0 | transfer ID | 0–31; the sender increments it per transfer on that channel, wrapping |

- **Single frame** (payload ≤ 7 bytes): payload, then tail with SOT = EOT = 1. DLC = payload + 1.
- **Multi-frame** (payload 8–256 bytes): the payload is preceded by its **CRC-16/CCITT-FALSE** (poly `0x1021`,
  init `0xFFFF`, no reflection, no final XOR; `"123456789"` → `0x29B1`), low byte first. That stream of CRC + payload
  is cut into 7-byte pieces, each followed by a tail byte. Only the last frame may be shorter than 8 bytes.

### Receiving

A receiver keeps one reassembly buffer per (channel ID) it cares about, i.e. per channel and sender.

- A frame with SOT starts a new transfer (dropping any unfinished one).
- A frame is appended only if its transfer ID matches and its toggle is the expected one; otherwise the transfer is
  dropped.
- More than **100 ms** between frames of one transfer drops it.
- On EOT the CRC is checked over the payload; a bad CRC or more than 256 bytes drops it.
- A transfer with the same transfer ID as the last completed one from that sender on that channel, arriving within
  1 s, is a duplicate and is ignored.

### Sending

- A node finishes one transfer on a channel before starting the next on that same channel. Different channels may
  interleave.
- **Delivery with retry.** Today every frame is single-shot, which is right for periodic data but loses a frame
  whenever it loses arbitration. Transfer frames are sent **with retry: up to 4 attempts within 20 ms**, then
  given up (the transfer is abandoned, and the state rule above repairs it). Either hardware retransmission cut
  off by that deadline, or a software resend after an arbitration-loss or error, is acceptable, as long as frames of a
  transfer leave in order. The legacy periodic frames stay single-shot. *(To confirm on the bench before firmware:
  how the ESP32 TWAI reports arbitration loss in single-shot mode.)*
- **Raw** channels (AUDIO_STREAM, BULK_DATA) have no tail byte: they are paced streams with their own sequence
  numbers, sent single-shot, with loss handled by the stream (below).

### Payload

- Byte 0 is the **opcode**. `0x00`–`0x7F` are broadcast messages. `0x80`–`0xFF` are **addressed**: byte 1 is the
  destination node (`0x00` = every node), and every node that is not the destination ignores it. The sender is in
  the CAN ID, so it is never repeated in the payload.
- Integers little-endian, as on the rest of the bus. `s32` "none" = `0x80000000`.
- Text is UTF-8 without a terminator unless a message says otherwise; the dash fonts cover ASCII and `°`, other
  characters show as `?`. Lengths come from the transfer length.

## NET (`0x3C0 + node`)

Discovery. Every node that speaks the protocol implements this channel.

| Op | Name | Payload after the opcode |
| --- | --- | --- |
| `0x01` | ANNOUNCE | protocol version u8 (1), node type u8, product u16, firmware version u16, unique ID u32 (from the MAC), description generation u8, produces u32, consumes u32, flags u8, name (≤ 24 bytes) |
| `0x81` | QUERY | dest u8, what u8: bit0 ANNOUNCE, bit1 sensor descriptions, bit2 alert descriptions, bit3 UI item descriptions |
| `0x82` | IDENTIFY | dest u8, seconds u8: blink an LED / beep so a person can find the board (0 = stop) |
| `0x83` | RESTART | dest u8, magic u8 `0x5A` (from the center or a service tool only) |
| `0x84` | RAIL | dest u8 (the rail master: the audio card, or a card asking it), slot u8 (sender's), state u8, sample rate / 10 u16 (see [The I2S rail](#the-i2s-rail)) |
| `0x10` | — | reserved (was WIFI OFFER) |
| `0x11` | GATEWAY STATUS | gateway → all, 1 Hz and on change: uplink u8, signal bars u8, RSSI s8 (dBm), technology u8, reserved u8 (was Wi-Fi clients), data this month u16 (MB), reserved u8 (was the Wi-Fi credentials generation), flags u8 (see [Network gateway](#network-gateway)) |
| `0x8A` | — | reserved (was WIFI QUERY) |
| `0x8B` | GATEWAY CMD | dest u8, command u8, argument u16 |
| `0x8C` | — | reserved (was WIFI SHARE) |

- **produces / consumes** are bitmaps by channel number (bit 1 = ALERT, 9 = SENSOR, 19 = UI, …). The node that
  consumes AUDIO_CMD is the audio sink; the node that consumes ALERT and SENSOR descriptions is the center. Nobody
  hard-codes node numbers for services.
- A node sends ANNOUNCE and all its descriptions at boot, and again when QUERYed. After the center boots it sends
  QUERY to `0x00` with all bits set; nodes answer after **(node × 8) ms**, so they don't all talk at once.
- **Description generation** bumps whenever any of the node's descriptions change (a firmware update, a setting that
  renames something). Receivers may cache descriptions keyed by (unique ID, generation) and skip asking again.
- **ANNOUNCE flags**: bit0 the node's settings are at factory defaults (never changed, or just reset). The center
  uses it to restore a backup (see [Settings backup](#settings-backup)). Bits 1–3: the slot the node sits in on a
  rev 2 board (1–6, read from the slot ID pin), 0 = not in a slot (cable port, rev 1 carrier, a screen).
- The unique ID also catches two carriers set to the same node number: the center sees two ANNOUNCEs with one node
  number and different unique IDs, and raises an alert.
- Node types extend the heartbeat's: 0 display, 1 audio bridge, 2 console, 3 sensor node, 4 input device, 5 audio
  sink, 6 output / relay node, 7 network gateway, 8 voice assistant, 9 radio / media source, 0x7F service tool. Products are listed under [Registries](#registries).

### The I2S rail

On a rev 2 board every slot shares one I2S bus (`docs/hardware/accessory_ports.md`, I2S rail rules). The audio card is
its only clock master and drives BCK, WS, MIC and MCLK whenever it is running. Any card may listen. **RET** (audio
back to the audio card) has one owner at a time, handed out over RAIL:

1. A card that wants to talk sends RAIL to the audio card with state 0 (request).
2. The audio card answers with state 1 (granted) or 2 (denied: someone else holds RET). Only after a grant does the
   card enable its RET driver. The audio card sends the current rate in every answer; a card that cannot follow it
   resamples or declines.
3. The holder sends state 2 (release) when it is done. The audio card can withdraw a grant (state 2 to the holder,
   for a higher-priority talker); the holder stops driving within 10 ms. A holder that loses the audio card's
   heartbeat stops at once, and the audio card frees RET when the holder's heartbeat stops.

| state | Meaning |
| --- | --- |
| 0 | request (card → audio card) |
| 1 | granted (audio card → card) |
| 2 | release / denied / withdrawn (either way) |

A voice card is the first user: it listens on MIC all the time (wake word) and asks for RET only while it speaks.

### Network gateway

A card with an internet uplink (an LTE card) announces as node type 7, does the online work itself and broadcasts the
results on [ONLINE](#online-0x360--node). The cluster never uses Wi-Fi: nothing joins a network, and CAN carries the
results, the gateway's status and its commands.

| Field | Values |
| --- | --- |
| uplink | 0 off, 1 searching, 2 registered, 3 data up |
| technology | 0 none, 2 3G, 3 LTE, 4 5G, 5 LTE-M / NB-IoT |
| flags (STATUS) | bit0 reserved (was access point on), bit1 roaming, bit2 data cap reached |
| command | 4 data cap in MB (argument); 1, 2, 3 and 5 reserved (were Wi-Fi access point on / off, new password, forget the shared network) |

- The same pattern serves any card that offers a service: it says on CAN what it has, so nothing has to search for it.

## SYSTEM (`0x060 + node`)

What the car and the dash are doing, for everyone. Only the center sends it in v1.

| Op | Name | Rate | Payload after the opcode |
| --- | --- | --- | --- |
| `0x01` | TIME | every 10 s + when set | UTC seconds u32, offset to local time in 15-minute steps s8, source u8 (0 none, 1 GPS, 2 reserved (was NTP), 3 RTC) |
| `0x02` | VEHICLE | 5 Hz | flags u16, backlight % u8, road speed km/h ×10 u16 (`0xFFFF` unknown) |
| `0x03` | PREFS | on change + every 5 s | units u8, theme u8, clock u8 (0 12 h, 1 24 h) |
| `0x04` | POWER | on the event, 3× 100 ms apart | event u8 (1 ignition off, 2 ignition back on), seconds until the rail may drop u8 |

VEHICLE flags: bit0 ignition on, bit1 engine running, bit2 headlights on, bit3 in reverse, bit4 moving, bit5 sensor
data live, bit6 night mode (screens dimmed). Accessories with LEDs follow backlight % and night mode.
PREFS units: bit0 distance / speed metric, bit1 temperature °C, bit2 pressure kPa (else psi). Accessories never need
the tire size, the gear or the sensor ADC maths: the center does those and sends results.

## ALERT (`0x020 + node`)

**Implemented on the center** (`firmware/elc_can/src/elc_alerts.cpp`, host-tested; `center_alerts.cpp`): the alert
manager (collecting, chimes, acknowledgement, source lost, SUMMARY) and the center's own warnings below. **Not yet:**
the banner and list on the menu screen, acknowledging from the menu screen (Select or a tap; the bench uses the
center's `alerts ack`), and alerts from other nodes' SENSOR thresholds (flag bit2).

The dash has one alert system. The center's own warnings (oil pressure, temperature, sensor faults) are published
on this channel from node `0x01` exactly like an accessory's, so a buzzer or lamp carrier sees all of them.

| Op | Name | Sender | Payload after the opcode |
| --- | --- | --- | --- |
| `0x01` | DEFINE | any | alert u8 (0–31), severity u8, flags u8, icon u8, sound u16, text (≤ 48 bytes) |
| `0x02` | STATE | any | active u32 (bit n = alert n active), description generation u8 |
| `0x03` | SUMMARY | center | highest active severity u8, highest unacknowledged severity u8, active count u8, top alert node u8, top alert u8 |
| `0x81` | ACK | center | dest u8, alert u8: the driver acknowledged it |

- An alert is identified by **(node, alert number)**; each node has up to 32.
- A node that defines alerts sends **STATE at 1 Hz, always**, and at once on any change. If a node's STATE stops for
  3 s, its alerts stay listed but greyed as "source lost", and the center raises its own advisory ("Oil cooler
  module not responding", from the node's ANNOUNCE name).
- DEFINE may be re-sent at any time to change text (e.g. "Trans temp 243 °F"); bump the generation byte in STATE so
  a receiver that missed it asks (QUERY bit2).
- **Flags**: bit0 needs acknowledgement, bit1 latching (stays listed until acknowledged even after it clears), bit2
  silent (no sound whatever the severity), bit3 reserved (0).
- **Sound**: `0x0000` = the default for the severity, `0xFFFF` = none, anything else a sound from the
  [registry](#sounds).
- ACK goes to the alert's source so it can react (stop its own buzzer). The center tracks acknowledgement itself;
  the source does not have to answer.
- SUMMARY is for simple nodes (a master-caution lamp on the console) that do not want to track every alert.

Alerts show on the **menu screen** only (the interface screen, `0x210`: the one that has the on-screen menu),
wherever the menu has been put. The gauges on the other two screens are never covered.

| Severity | Name | On the menu screen | Sound (default) |
| --- | --- | --- | --- |
| 0 | info | toast for 4 s, no list entry | none |
| 1 | advisory | toast until cleared or acknowledged, listed | soft chime once |
| 2 | caution | amber banner, listed | caution chime once |
| 3 | warning | red overlay, listed | warning chime, repeating until acknowledged |

Accessories may not suppress another node's alert. The driver acknowledges with Select, a tap on the banner, or
the alert list in the menu.

**Chimes.** The center plays them on the sink, as node `0x01`. An advisory or a caution plays its sound once when it
becomes active (or its severity rises), at priority 2 / 3. A warning's sound repeats (PLAY repeat 0, priority 4)
while any warning is active and unacknowledged, and stops (STOP) once all are acknowledged or clear; if the sink
reports DONE for it while it should still ring, it is played again. An acknowledged alert never chimes again in the
same activation; one that clears and comes back is new.

### Center alerts (node `0x01`)

The center raises these from the sensor pack, with the console's thresholds. Numbers are stable.

| Alert | Severity | Text | Raised | Cleared |
| --- | --- | --- | --- | --- |
| 0 | advisory | Sensor data lost | no sensor frames for 2 s after frames with the ignition on | frames again |
| 1 | warning, ack | Low oil pressure | under 8 psi for 2 s with the engine running (ignition on, over 400 rpm) | 10 psi, or not running |
| 2 | caution | Engine hot | over 230 °F for 3 s | under 225 °F for 3 s |
| 3 | warning, ack | Engine overheating | over 245 °F for 3 s | under 240 °F for 3 s |
| 4 | caution | Not charging | running and the alternator not charging for 5 s | charging for 2 s |
| 5 | advisory | Low fuel | under 12 % for 10 s | over 15 % for 10 s |
| 6 | advisory | Odometer not stored | ODO_STORE fault | the fault clears |
| 7 | advisory | Sensor fault: … | a sender open or short (the text names it) | the fault clears |
| 8 | advisory | … not responding | a node's ALERT STATE stopped for 3 s (named from its ANNOUNCE; "N modules" for several) | STATE again |

A sender with an open or short fault never raises its reading's alert (1-3, 5). With no fresh sensor data, alerts
1-7 hold their state until data returns.

## SENSOR (`0x120 + node`)

Readings in known quantities, so the dash can convert, show, log and alarm on them without accessory-specific code.
The center publishes the car's own readings here as node `0x01` ([core sensors](#core-sensors-node-0x01)), so
every consumer, the console included, reads one format.

| Op | Name | Rate | Payload after the opcode |
| --- | --- | --- | --- |
| `0x01` | DEFINE | boot + on QUERY | sensor u8 (0–31), quantity u8, exponent s8, flags u8, period ms u16, display min s32, display max s32, caution low s32, caution high s32, warning low s32, warning high s32, name (≤ 20 bytes) |
| `0x02` | VALUE | its period | sensor u8, value s32, status u8 |

- **Value = raw × 10^exponent**, in the quantity's canonical unit ([registry](#quantities)). A transmission
  temperature with exponent −1 sends 1043 for 104.3 °C; the dash shows 219.7 °F if PREFS says so.
- **Status**: 0 ok, 1 not ready (warming up), 2 open circuit, 3 short circuit, 4 out of range, 5 fault. A value
  with status ≠ 0 shows as `--` with the reason in the menu.
- **Stale**: no VALUE for 3 × period (at least 500 ms) shows `--`.
- **Flags**: bit0 log to the SD card (the center's sensor log), bit1 offer as a gauge readout (it appears in the list
  of things a screen can show), bit2 the center raises alerts from the thresholds (below).
- **Thresholds** (`0x80000000` = none) colour the readout and, with flag bit2, make the **center** raise a caution or
  warning on the accessory's behalf ("Trans temp high"), so a simple sensor board needs no alert logic. An accessory
  that wants its own wording leaves bit2 clear and uses ALERT.
- **Period**: 50 ms minimum (20 Hz), 1000 ms for slow things. A VALUE is one frame, so 10 sensors at 10 Hz is about
  3 % of the bus.

### Core sensors (node `0x01`)

The center converts each sensor-pack frame (`sensor_uart.md`) and sends the results on `0x121`. These are the
readings for anything off the cluster: **the console reads its gauges' numbers here**, not from the legacy frames.

| Sensor | Reading | Quantity | Exponent | Period | Status from |
| --- | --- | --- | --- | --- | --- |
| 0 | road speed | speed (km/h) | −1 | 50 ms | |
| 1 | engine speed | rotational speed (rpm) | 0 | 50 ms | COIL_MISSING → 1 not ready |
| 2 | fuel level | percent | −1 | 200 ms | POT_OPEN / POT_SHORT → 2 / 3, its *_POWER bit → 5 |
| 3 | engine temperature | temperature (°C) | −1 | 200 ms | TEMP_OPEN / TEMP_SHORT → 2 / 3, its *_POWER bit → 5 |
| 4 | oil pressure | pressure (kPa) | 0 | 200 ms | PRESS_OPEN / PRESS_SHORT → 2 / 3, its *_POWER bit → 5 |
| 5 | charging | switch (1 = charging) | 0 | 200 ms | ALT_STUCK → 5 |
| 6 | ambient temperature | temperature (°C) | −1 | 1000 ms | |
| 7 | odometer | distance (m) | 0 | 1000 ms | ODO_STORE → 5; unknown → 1 |
| 8 | distance to empty | distance (m) | 0 | 1000 ms | unknown → 1 |
| 9 | fuel economy, instant | fuel economy (km/L) | −2 | 1000 ms | no economy → 1 |
| 10 | fuel economy, trip | fuel economy (km/L) | −2 | 1000 ms | no economy → 1 |

- About 70 frames/s, **~2 % of the bus**. With no sensor data for 400 ms (the gauges' NO DATA) every core sensor
  goes to status 1, and VEHICLE flag bit5 clears.
- The thresholds in their DEFINEs match the gauges' own redlines. Flag bit2 is clear: the center raises its warnings
  (oil pressure, temperature, sensor faults) itself on ALERT with its own wording.
- The legacy frames `0x100`–`0x104` stay, for the three screens only (they carry raw ADC values the gauge faces are
  calibrated against, and they are internal). A new consumer never uses them.

## UI (`0x260 + node`)

An accessory describes a small tree of **items**; the screen that shows the menu renders them under
**Menu → Accessories → (its ANNOUNCE name)**. No pixels cross the bus, and the accessory never has to know which
screen, theme or input device is in use.

| Op | Name | Sender | Payload after the opcode |
| --- | --- | --- | --- |
| `0x01` | ITEM | accessory | item u8, parent u8 (`0xFF` = top), kind u8, flags u8, exponent s8, min s32, max s32, step s32, then NUL-separated text: label, unit, options |
| `0x02` | VALUE | accessory | item u8, value s32 |
| `0x03` | TEXT | accessory | item u8, text (≤ 64 bytes) |
| `0x04` | RESULT | accessory | item u8, result u8, asker node u8 |
| `0x81` | SET | menu screen | dest u8, item u8, value s32 |
| `0x82` | VIEW | menu screen | dest u8, item u8 being shown (a group; `0xFE` = the accessory's top page, `0xFF` = closed) |

Kinds:

| Kind | What | Value |
| --- | --- | --- |
| 0 | group (a sub-page) | none |
| 1 | readout | value × 10^exponent, with the unit |
| 2 | toggle | 0 / 1 |
| 3 | number | min…max in steps, × 10^exponent, with the unit |
| 4 | choice | index into the options (one per line, `\n`-separated) |
| 5 | action (a button) | SET 1 triggers it |
| 6 | text readout | from TEXT |

Flags: bit0 read-only, bit1 ask to confirm before SET (actions like "Reset trip"), bit2 advanced (only under
Service), bit3 live (VALUE streams while viewed), bit4 a setting (the center backs it up).

- Up to 64 items per node, item numbers 0–254. ITEM descriptions are sent at boot and on QUERY bit3.
- **Settings follow the existing `0x212` pattern**: the menu screen sends SET; the accessory applies it, saves it
  itself and answers with VALUE and RESULT, even when nothing changed. Result: 0 ok, 1 out of range, 2 read-only, 3 busy, 4 unknown item. The menu shows the value from VALUE,
  never the one it asked for.
- **VIEW** is sent every 1 s while a page of the accessory is open. While the last VIEW is under 3 s old, the
  accessory sends VALUE / TEXT for the items under that group at up to 5 Hz; otherwise only on change. That keeps a
  page of live readouts off the bus when nobody looks at it.
### Settings backup

**Not implemented yet:** the center keeps no copy and restores nothing so far. This section is the design.

The accessory is the owner of its settings: it saves them itself and works without the center. The **center keeps
a copy on its SD card** so a replaced or re-flashed carrier gets them back.

- **Backing up.** The center stores every VALUE of an item with flag bit4, together with that item's kind and label,
  in `/accessories/<product>_<node>.json` (e.g. `0005_14.json`, hex), with the unique ID of the board it came from.
  Written when a value changes, at most once every 5 s per node, like `settings.json`.
- **Keyed by product and node**, not by board: a new board of the same product at the same node number is the same
  accessory in the car.
- **Restoring.** When a node ANNOUNCEs and a backup exists for its product and node, and once its ITEM descriptions
  are in:
  - same unique ID → nothing to do (the board has its own settings);
  - different unique ID and ANNOUNCE flag bit0 set (factory defaults) → the center restores automatically: a SET
    per backed-up item, from node `0x01`, one at a time, waiting for each RESULT;
  - different unique ID and bit0 clear (a used board moved in) → nothing is sent; the accessory's page in the menu
    offers **Restore saved settings** and **Keep this board's settings**.
- Only items whose number, kind and label still match are restored, so a firmware update that changes the items
  loses nothing but what changed. A value the accessory rejects (RESULT ≠ 0) is skipped and logged.
- **Service → Accessories** lists the backups, including nodes not on the bus, with **Forget** for each.
- If the center has no SD card, backup and restore are off; the accessories are unaffected.

### Not in v1

- Accessory-drawn screens or custom widgets. Pixels over CAN run at about one small image per second
  (see the album art in `can_bus.md`). Bar and dial kinds can be added later as new kind numbers.

## INPUT (`0x040 + node`)

Keys and knobs from any node: a steering-wheel button carrier or an IR receiver (not implemented yet; the console
drives its own OLED menu and sends AUDIO_CMD directly, so it does not send INPUT). The source
resolves timing (click vs long press), so receivers need no timers.

| Op | Name | Rate | Payload after the opcode |
| --- | --- | --- | --- |
| `0x01` | KEY | per event | key u8, gesture u8, sequence u8 |
| `0x02` | ENCODER | on change + every 1 s | encoder u8, position s16 (cumulative, wraps) |

- Gestures: 1 click (sent on release), 2 long press (sent once at 0.8 s held), 3 repeat (every 150 ms while held
  after a long press), 4 down, 5 up. A source sends at least clicks and long presses; 4 / 5 are for receivers that
  want raw timing.
- **Sequence** increments per KEY from that node, so a receiver ignores a retried duplicate.
- **ENCODER positions are cumulative**, so a lost frame loses no clicks: receivers act on the difference from the
  last position they saw from that node.
- Encoders: 0 navigation (Select), 1 volume, 2+ accessory-specific.

Who acts on what (every consumer listens and acts on its own keys; nobody routes input):

| Input | Acted on by |
| --- | --- |
| up, down, left, right, select, back, encoder 0 | the screen showing the menu (opens it on select when closed) |
| view | the center (cycle its view) |
| play/pause, next, previous, mute, volume ±, encoder 1 | the audio sink |
| select or back while an alert banner shows | the center (acknowledge) |
| F1–F8 | nothing by default; assignable to an accessory action in the menu (later) |

## AUDIO_CMD (`0x240 + node`)

The sink is the node that announces it consumes AUDIO_CMD (node type 5): the audio card (slot 1, node `0x14`), whose
firmware so far only plays the chimes, sounds 2-4 (`tools/cards_sim.py` simulates the rest). Commands may be sent to it by number or to `0x00` (the sink is the only one that acts on them).

| Op | Name | Sender | Payload after the opcode |
| --- | --- | --- | --- |
| `0x01` | STATUS | sink | volume u8 (0–100), flags u8, active sounds u8, stream owner node u8 (0 none), zones fitted u8, source u8, source node u8 |
| `0x02` | DONE | sink | node u8, sound u16: a sound that node started has finished |
| `0x03` | STREAM_GRANT | sink | node u8 (0 = refused / closed), codec u8, rate u8 |
| `0x04` | NOW PLAYING | the active source | source u8, flags u8, preset u8 (0 none, 1–12), signal u8 (0–5 bars, `0xFF` unknown), line 1 length u8, line 1 (≤ 32 bytes), line 2 length u8, line 2 (≤ 64 bytes) |
| `0x81` | PLAY | any | dest u8, sound u16, zones:4 \| priority:4 u8, level % u8, repeat u8 (0 = until STOP) |
| `0x82` | STOP | any | dest u8, sound u16 (`0xFFFF` = every sound this node started) |
| `0x83` | VOLUME | any | dest u8, action u8 (0 set, 1 step, 2 mute toggle), value s8 |
| `0x84` | MEDIA | any | dest u8, action u8 (see below), value u8 |
| `0x85` | STREAM_OPEN | any | dest u8, codec u8, rate u8, zones:4 \| priority:4 u8, level % u8 |
| `0x86` | STREAM_CLOSE | stream owner | dest u8 |
| `0x87` | SOURCE | any | dest u8 (the sink), source u8 (0 = the next one, `0xFF` = off, else a source code) |

- STATUS at 1 Hz and on change. Flags: bit0 muted, bit1 music playing, bit2 music ducked.
- **VOLUME is the master volume**: the sink scales its output after mixing, so it covers every source and the
  sounds. Bluetooth's own (AVRCP absolute) volume is held at full by the sink, which puts it back when the phone's
  slider moves; the phone's slider is never the car's volume.
- **Sources.** The sink owns which source plays. Source codes: 0 none (**off**), 1 Bluetooth (the sink's own A2DP, or
  the audio bridge), 2 radio, 3 aux in. **source node** is the node that provides it (the sink itself for Bluetooth and
  aux; 0 when off). SOURCE with 0 steps to the next one in the order Bluetooth, radio (once its ANNOUNCE is heard, node
  type 9), off, Bluetooth; `0xFF` turns the source off. **Off** plays no source (Bluetooth paused, the radio muted
  with RET released) while the sink stays up: its sounds and the alerts' chimes still play.
  A source learns it was chosen from STATUS: the radio powers on and asks for RET when STATUS names it, and mutes and
  releases RET when it does not. Bluetooth pauses when the sink leaves it and plays again when it comes back (if it was
  playing): the sink does that for its own A2DP, the console for the audio bridge (`can_bus.md`, phone command 14). The console's left knob **hold** sends SOURCE with an explicit code, stepping round Bluetooth, radio (when heard) and
off (`0xFF`); its click is play / pause (Bluetooth), mute (radio), or audio back on when the source is off.
- **MEDIA actions:** 1 play/pause, 2 play, 3 pause, 4 next, 5 previous, 6 tune up, 7 tune down (one grid step),
  8 next preset, 9 previous preset, 10 store preset (value = slot 1–12, 0 = the next free one, else the oldest),
  11 seek up, 12 seek down, 13 recall preset (value = slot 1–12); value is 0 where an action takes none. The sink acts on them for its own source and
  forwards them, unchanged and addressed to the source node, when another node is the source. A source ignores what
  it cannot do (Bluetooth: 6 / 7 = next / previous, 8–12 nothing; the radio: play/pause = mute).
- **NOW PLAYING** is sent by the active source on change and every 2 s, for the console and the media screen.
  Flags: bit0 stereo, bit1 RDS, bit2 muted, bit3 seeking. The radio's line 1 is the frequency and station name
  ("94.7 FM  THE WAVE"), line 2 its radio text. Bluetooth keeps its own frames (`can_bus.md`, `0x400`–`0x403`).
- Zones: bit0 front left, bit1 front right, bit2 rear left, bit3 rear right; 0 = the sink's default.
- **Priority** 0 UI click, 1 accessory, 2 advisory, 3 caution, 4 warning. A sound with priority ≥ 2 ducks the
  music; a higher-priority sound interrupts a lower one in the same zones. Accessories use 0–1; 2–4 are for the
  center's alerts.
- A sound is identified by (node, sound number), so STOP from one node never stops another's.
- **Parking-sensor example:** PLAY the tick with repeat 0, change its rate by sending PLAY again with a different
  sound (fast / medium / slow tick), STOP when clear. No streaming needed.

## AUDIO_STREAM (`0x300 + node`, raw)

For audio an accessory generates and the sink cannot have stored (a voice prompt made on the fly, an intercom).
**Voice quality only, one stream at a time, granted by the sink.**

1. Send STREAM_OPEN (AUDIO_CMD). The sink answers STREAM_GRANT naming the node it granted, or 0.
2. Send stream frames on `0x300 + node`: 8 bytes, **no tail byte**:
   - byte 0: sequence u8 (wraps)
   - normal frame: bytes 1–7 = 14 IMA-ADPCM samples, low nibble first
   - every 32nd frame (sequence % 32 == 0) is a **sync frame**: predictor s16, step index u8, then 8 samples in
     bytes 4–7. The sync state applies before its samples.
3. Send STREAM_CLOSE when done.

- Codec 1 = IMA-ADPCM, mono. Rate 0 = 8 kHz (about 580 frames/s, **~16 % of the bus**), 1 = 16 kHz (about 1,160
  frames/s, **~31 %**).
- The sink buffers 60 ms. A missing sequence number plays as silence until the next sync frame. No frames for
  200 ms closes the stream; so does 30 s without STREAM_CLOSE unless the sink allows longer.
- While a 16 kHz stream is open, BULK and album art halve their pace so the bus stays under about 70 %.

## BULK (`0x320 + node` control, `0x340 + node` data)

Moving objects larger than a transfer: a sound clip to the sink, later a firmware image to an accessory. It
generalises the album-art mechanism in `can_bus.md` (receiver-driven, re-ask for what is missing), which stays
as it is.

| Op | Name | Sender | Payload after the opcode |
| --- | --- | --- | --- |
| `0x01` | INFO | object owner / receiver of a write | to node u8, tag u8, session u8 (0–7, `0xFF` = refused), size u32, CRC-32 u32 |
| `0x81` | READ | reader | dest u8, tag u8, kind u8, object u16 |
| `0x82` | WRITE | writer | dest u8, tag u8, kind u8, object u16, size u32, CRC-32 u32 |
| `0x83` | WANT | receiving side | dest u8, session u8, first chunk u24, credit u8 (chunks it can take now) |
| `0x84` | END | receiving side | dest u8, session u8, result u8 (0 ok, 1 CRC mismatch, 2 no space, 3 aborted) |

Data frames on `0x340 + sender`, **no tail byte**: bytes 0–2 = session (top 3 bits) and chunk number (21 bits),
bytes 3–7 = 5 bytes of the object (chunk n = bytes 5n…5n+4). Up to 10 MB per object.

- The **sender of the data** picks the session (it owns the data IDs), returns it in INFO with the asker's tag, and
  sends only chunks the receiver has asked for with WANT. The receiver asks for more before its credit runs out and
  re-asks from its first missing chunk after 300 ms of quiet.
- The object's CRC-32 (IEEE, as in zlib) is checked by the receiver before END ok.
- Pace: all BULK data, together with album art, at most one frame per 0.8 ms (about a third of the bus, ~6 KB/s).
- Kinds: 1 sound clip (object = slot, 16-bit PCM or IMA-ADPCM WAV, ≤ 64 KB), 2 firmware image (the update
  procedure is a later profile; v1 fixes only the transport), 3 file on the center's SD card, `0x80`+ product-specific.

## ONLINE (`0x360 + node`)

Internet results, for everyone. **The dash does as much of its online work as possible over CAN**: the node with the
internet connection (the LTE gateway, `docs/hardware/lte_card.md`) does the lookups itself and broadcasts only the
answers, which are a few dozen bytes. The cluster never uses Wi-Fi.

| Op | Name | Sent | Payload after the opcode |
| --- | --- | --- | --- |
| `0x01` | PLACE | on change, and every 30 s | serial u8, flags u8, speed limit u8, country code (2 bytes, ISO 3166), street length u8, street, locality length u8, locality, region length u8, region |
| `0x02` | WEATHER | every 15 min, and when the place moves more than 10 km | observed at u32 (UTC s), temperature s16 (0.1 °C), feels like s16 (0.1 °C), WMO weather code u8, humidity u8 (%), wind u16 (0.1 m/s), wind from u16 (degrees), rain chance u8 (% next 6 h), today's high s16, today's low s16 (0.1 °C), sunrise u32, sunset u32 (UTC s) |
| `0x03` | TIMEZONE | on change, and every 5 min | UTC offset s8 (15-minute steps), flags u8 (bit0 daylight saving now), IANA name length u8, IANA name (e.g. `America/Chicago`) |
| `0x04` | MESSAGE | when one arrives | id u8, source u8 (1 SMS, 2 online service), received at u32 (UTC s), sender length u8, sender, text length u8, text (≤ 160 bytes) |
| `0x05` | ONLINE STATUS | 1 Hz | flags u8 (bit0 internet up, bit1 place valid, bit2 weather valid, bit3 time zone valid), seconds since the last place lookup u16, lookups this hour u8, last error u8 (0 none, 1 no fix, 2 no data connection, 3 service error, 4 rate limited) |
| `0x81` | REFRESH | from anyone | dest u8 (the gateway), what u8 (bit0 place, bit1 weather, bit2 time zone): look it up now and broadcast |
| `0x82` | MESSAGE READ | from the screen that showed it | dest u8 (the gateway), id u8 |

- Strings are UTF-8, no terminator, each with its length byte in front. Street up to 48 bytes, locality and region up to
  32 each.
- **PLACE flags:** bit0 valid, bit1 from cache (no data connection right now), bit2 the speed limit is in mph (else
  km/h); speed limit 0 = unknown. **serial** goes up with each new place, so a receiver can skip an unchanged repeat.
- **Weather** codes are the WMO codes Open-Meteo uses (0 clear, 1–3 cloud, 45 / 48 fog, 51–67 rain, 71–77 snow, 80–82
  showers, 95–99 thunder). Unknown values: `0x7FFF` for s16, `0xFF` for u8.
- **Where the input comes from:** the gateway hears the GPS card's position (`can_bus.md` `0x222`) like every other
  node. Nobody asks it for each lookup.
- **When it looks up the place:** when the car has moved more than 50 m since the last lookup and at least 10 s have
  passed, and once on boot. Stopped, it does not look up again. That stays well inside OpenStreetMap Nominatim's limit
  (one request per second) and to about 1–2 MB of data per hour of driving.
- **Services:** reverse geocoding from OpenStreetMap **Nominatim** (the gateway sends its own User-Agent as Nominatim's
  policy asks). Weather and the time zone from **Open-Meteo** (no key; one call
  with `timezone=auto` returns both). The speed limit from OpenStreetMap's `maxspeed` tags through Overpass, optional
  (0 when not looked up). Each service is a setting on the gateway, so another provider can replace it.
- **The center** sends the time on `0x220` (from GPS). It does no lookups of its own; `0x221` (street) is reserved, and
  a screen that wants the place reads PLACE here.
- **Messages:** the A7670 can receive SMS. The gateway forwards each one as MESSAGE (shown like an alert on the menu
  screen) until a screen sends MESSAGE READ. Sending SMS from the dash is not in v1.
- **Settings** (lookup interval, units, the APN, which services) are the gateway's UI items in the
  menu, as for any accessory (see [UI](#ui-0x260--node)).
- A gateway with no data connection keeps sending ONLINE STATUS and its cached PLACE (flags bit1), so the screens can
  say why nothing updates.

## LOG (`0x3E0 + node`)

| Op | Name | Payload after the opcode |
| --- | --- | --- |
| `0x01` | LINE | level u8 (0 debug, 1 info, 2 warning, 3 error), text (≤ 120 bytes) |

The center writes lines to the SD card's log with the node's name and shows them on its serial console
(`can log`). At most 10 lines per second per node; debug lines only when asked for (a UI toggle on the accessory).

## Registries

Append-only. A number, once given, never changes meaning. Numbers `0x80` and above in u8 registries (`0x8000` in
u16) are free for a product's own use.

### Products

| Product | What |
| --- | --- |
| `0x0000` | unspecified |
| `0x0001` | round display (center / left / right) |
| `0x0002` | audio bridge (classic ESP32, A2DP) |
| `0x0003` | console (OLED + knobs) |
| `0x0004` | audio carrier (four-channel); rev 2: the audio card |
| `0x0005` | GPS / clock / motion card |
| `0x0006` | LTE gateway card |
| `0x0007` | AI voice card |
| `0x0008` | sensor card (the sensor pack, node `0x04`; calibration on its UI page: `sensor_calibration.md`) |
| `0x0009` | AM/FM radio card (`docs/hardware/radio_card.md`) |

### Quantities

| # | Quantity | Canonical unit | | # | Quantity | Canonical unit |
| --- | --- | --- | --- | --- | --- | --- |
| 0 | unitless | — | | 10 | distance | m |
| 1 | temperature | °C | | 11 | volume | L |
| 2 | pressure | kPa (gauge) | | 12 | flow | L/h |
| 3 | voltage | V | | 13 | angle | ° |
| 4 | current | A | | 14 | switch | 0 / 1 |
| 5 | rotational speed | rpm | | 15 | count | — |
| 6 | speed | km/h | | 16 | frequency | Hz |
| 7 | percent | % | | 17 | acceleration | g |
| 8 | lambda | λ | | 18 | humidity | % RH |
| 9 | air-fuel ratio | :1 (gasoline) | | 19 | duration | s |
| | | | | 20 | fuel economy | km/L |

### Keys

| Code | Key | | Code | Key |
| --- | --- | --- | --- | --- |
| 1 | up | | 16 | play / pause |
| 2 | down | | 17 | next |
| 3 | left | | 18 | previous |
| 4 | right | | 19 | volume up |
| 5 | select | | 20 | volume down |
| 6 | back | | 21 | mute |
| 7 | view | | 32–39 | F1–F8 |
| 8 | home (close the menu) | | | |

### Icons

0 generic, 1 temperature, 2 oil, 3 battery / charging, 4 fuel, 5 door / tailgate, 6 tire, 7 lamp / bulb,
8 engine, 9 brake, 10 information, 11 service (wrench), 12 connection (a node lost).

### Sounds

Built into every sink, `0x0001`–`0x00FF`:

| # | Sound | | # | Sound |
| --- | --- | --- | --- | --- |
| 1 | UI click | | 6 | turn-signal tick |
| 2 | soft chime (advisory) | | 7 | parking tick, slow |
| 3 | caution chime | | 8 | parking tick, medium |
| 4 | warning chime | | 9 | parking tick, fast |
| 5 | key-on / welcome | | 10 | continuous tone |

The audio card has 2-4 so far (`chimes.cpp` in the audio bridge sketch), synthesised as struck bells so they play at
whatever rate the output runs. Each length is one play including its trailing silence, so a repeated sound keeps its
rhythm:

| # | Sound | Length |
| --- | --- | --- |
| 2 | soft chime: one mellow strike on A5 (880 Hz) | 1.2 s |
| 3 | caution chime: ding-dong, E6 then C6 320 ms later | 1.5 s |
| 4 | warning chime: two triplets of bright A6 (1760 Hz) strikes, a second apart; repeat 0 until acknowledged | 2.0 s |

They play at the card's own **Chime volume** setting (1-15, default 8; Menu → Accessories → Audio card, a UI number
item, saved on the card), not at the music's volume, so turning the music down never quietens an alert. Changing it
plays the soft chime at the new level. PLAY's level % scales from there. They mix over the music (ducked about 10 dB under an alert) or the
phone call, or play alone when neither is running.

`0x0100`–`0x01FF`: uploaded clips (BULK kind 1, object = low byte). An uploaded clip belongs to the sink, not the
uploader; the uploader checks the slot's CRC (BULK READ) before uploading again.

## Examples

### A transmission-temperature card (in slot 1: node `0x14`)

An ESP32-C3 with a thermistor. It needs no code in the dash.

At boot (after its heartbeat with flags bit0 set):

- NET ANNOUNCE: node type 3, name "Trans temp", produces SENSOR + ALERT + UI, consumes SYSTEM.
- SENSOR DEFINE sensor 0: temperature, exponent −1, flags log + gauge + center-alerts, period 200 ms, display
  400…1500 (40–150 °C), caution high 1100 (110 °C), warning high 1250 (125 °C), no lows.
- UI ITEM 0: number "Offset", unit "°C", −50…50, exponent −1, flag bit4 (a calibration trim: it saves it
  itself and the center backs it up, so a replacement board gets the same trim).

Then 5 times a second, SENSOR VALUE sensor 0 = 1043 (104.3 °C), status ok, transfer ID 7, on ID `0x134`
(`0x120 + 0x14`), one frame:

```
0x134  [8]  02 00 13 04 00 00 00 C7
            op sensor value=1043  status tail(SOT|EOT|TID 7)
```

At 110 °C the center raises "Trans temp high" (caution) from node `0x01`; at 125 °C a warning.

### A multi-frame transfer: ALERT DEFINE

The card (node `0x14` in slot 1) defines its own alert 0 instead: caution, needs acknowledgement, icon temperature, default sound, text
"Trans fluid hot". Payload (22 bytes): `01 00 02 01 01 00 00` + the text. CRC-16 of the payload = `0xC15F`. Transfer
ID 3, on ID `0x034` (`0x020 + 0x14`):

```
0x034  [8]  5F C1 01 00 02 01 01 83     CRC lo, CRC hi, op, alert, severity, flags, icon | tail SOT, toggle 0
0x034  [8]  00 00 54 72 61 6E 73 23     sound u16, "Trans"                              | tail toggle 1
0x034  [8]  20 66 6C 75 69 64 20 03     " fluid "                                       | tail toggle 0
0x034  [4]  68 6F 74 63                 "hot"                                           | tail EOT, toggle 1
```

and then every second, and at once on change, ALERT STATE with alert 0 active (generation 1, transfer ID 12):

```
0x034  [7]  02 01 00 00 00 01 CC
```

### A steering-wheel button carrier

Produces INPUT only. Its KEY frames (next, previous, volume ±, select, back) drive the menu and the media with no
change to any other firmware: the audio sink and the menu screen already listen to INPUT.

## Bus budget

| Traffic | Share of the bus |
| --- | --- |
| Legacy sensor, cluster, heartbeats, audio transport (today) | ~5 % |
| Core sensors on SENSOR (center) | ~2 % |
| SYSTEM, ALERT, INPUT, UI, NET, LOG, ONLINE | < 1 % |
| Accessory sensors (budget: 20 values at 10 Hz) | ~6 % |
| BULK and album art together (paced, only while transferring) | ≤ 34 % |
| AUDIO_STREAM, one at a time (8 kHz / 16 kHz) | 16 % / 31 % |

Design ceiling about 70 %, which the stream rule under AUDIO_STREAM keeps. Higher-priority channels are unaffected
by bulk and stream traffic anyway, because those have the higher IDs.

## What an accessory must implement

| Level | Implements |
| --- | --- |
| Minimum | heartbeat (flags bit0), NET ANNOUNCE / QUERY / IDENTIFY |
| Per service | the channels it announces as produced or consumed, with their repeat rules |
| Recommended | LOG, and SYSTEM POWER (save state before the rail drops) |

## Order of work (when firmware starts)

1. Transport (tail-byte framing, reassembly, retry) and NET in `firmware/elc_can`, with host tests; heartbeat flag.
2. SYSTEM and LOG from the center.
3. SENSOR and ALERT: the core sensors from the center, the center's alert manager and the menu screen's banner and
   list; the center's own warnings move onto ALERT.
4. INPUT and the console's readings from SENSOR (with the console firmware), and AUDIO_CMD (on the audio bridge
   first, then the audio carrier).
5. UI items in the menu, then the settings backup on the center's SD card.
6. BULK (sound upload), then AUDIO_STREAM, then firmware update over CAN as its own profile.

## Open questions

- **Retry**: how the TWAI controller reports arbitration loss with single-shot set, and whether bounded hardware
  retransmission is simpler than resending in software. Measure on the bench before step 1.

## Decided

- Alerts show on the menu screen only.
- The center publishes the core readings on SENSOR; the console (and every other consumer) reads them there.
- Accessory settings are backed up on the center's SD card and restored to a replacement board.
