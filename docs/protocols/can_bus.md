# CAN bus

The DigiDash board links the three displays, accessory carrier boards (on its accessory ports) and the console
(OLED + knobs, on its own board at the end of a short cable) on one CAN bus. **CAN is the link** for everything
except the sensor UART, in the car and on the bench: there are no bench builds, and a bench without transceivers
reaches the bus through the USB CAN link (`tools/can_relay.py`, [`usb_link.md`](usb_link.md)). Ports, carriers and
their rules:
[`docs/hardware/accessory_ports.md`](../hardware/accessory_ports.md).

**Accessories:** the public interface for carrier boards (alerts, sensors, menu items, input, sounds, discovery) is
the channel protocol in [`can_channels.md`](can_channels.md) (specification only, no firmware yet). The messages
below are the cluster's own traffic.

Shared code: `firmware/elc_can` (IDs in `elc_can_ids.h`, codecs in `elc_can_codec.h`, driver in `elc_can.h`).
Host tests: `firmware/elc_can/test/host/run_tests.sh`.

The sensor UART fan-out (`sensor_uart.md`) is unchanged: the gauges never depend on CAN. The center re-sends
each sensor frame onto CAN so accessories get the same data.

## Physical layer

- 500 kbit/s, 11-bit IDs, one **MCP2562-E/P** (DIP-8) per node: VDD = 5 V, Vio = 3.3 V, STBY tied low. The three
  screens' transceivers are on the DigiDash board (U5, U9, U10); every accessory carries its own.
- 120 Ω at the two ends only: **R35 at the left screen (U5)** and **R36 at the right screen (U10)**. Along the bus
  between them sit the four accessory header positions (J18–J21), the two cable ports (J17, J22) and the center (U9).
  Accessories never terminate, so the bus is complete with any of them unplugged. D2 (NUP2105L) clamps ESD on the pair.
- Each screen TXD has a 10 kΩ pull-up to 3.3 V, so a screen that is booting holds the bus recessive.
- **J16** (CANH, CANL, GND) is a tap for a USB-CAN adapter.

| Node | Board part | CAN TX | CAN RX | Node number |
| --- | --- | --- | --- | --- |
| Left screen (speedo) | J5 → U5 | GPIO34 (header pin 31) | GPIO36 (header pin 23) | `0x02` |
| Center screen | J1 → U9 | GPIO34 (header pin 31) | GPIO36 (header pin 23) | `0x01` |
| Right screen (TSPS) | J4 → U10 | GPIO34 (header pin 31) | GPIO36 (header pin 23) | `0x03` |
| Audio bridge (carrier on a header position) | on the carrier | GPIO32 | GPIO33 (via 1 kΩ) | `0x10` (straps: `0x11`, `0x12`) |
| Console (ESP32-S3, cable port J17) | on the console | set in its firmware | | `0x13` |
| Further accessories | on the carrier | | | `0x14`–`0x1B` |

The three screens run the same firmware; each takes its node number from its position strap (header pin 17 = GPIO4:
J5 to GND = left, J4 to 3V3 = right, J1 open = center; see `WIRING_PINOUT.md`). A screen that hears a heartbeat with
its own node number warns on its console: two boards claim the same position.

GPIO34/36 are P4 strapping pins but safe here: boot mode is set by GPIO35 (left pulled up), GPIO36 only
matters when GPIO35 is low, and the TXD pull-up holds GPIO34 high during reset.

Build flags: `ELC_CAN_TX_PIN` / `ELC_CAN_RX_PIN` (set in each `platformio.ini`; `-1` turns CAN off).

The first board revision had three ESP32-DevKitC slots (U3 / U11 / U12, slot numbers from GPIO13 / GPIO23 straps) and
the I2S amplifier sockets J14 / J15 instead of the ports; the audio bridge firmware works on either (open straps =
node `0x10`).

### Console cable (J17)

The console is its own board: an ESP32-S3 dev board (its TWAI controller drives one MCP2562), the SSD1322 256×64
OLED on SPI, the Select and Volume encoders and the Back and View keys. One cable runs from it to **cable port J17**
on the DigiDash board, a through-hole JST PH 4-pin (S4B-PH-K) with the DroneCAN pin order (J22 is a second,
spare cable port):

| PH pin | Signal |
| --- | --- |
| 1 | +5 V (from the DigiDash rail through its own 0.5 A PTC) |
| 2 | CANH |
| 3 | CANL |
| 4 | GND |

- **Termination stays on the DigiDash board** (R35 at U5, R36 at U10), so the bus works the same with the console
  unplugged. **The console board has no 120 Ω.** Check: ~60 Ω between CANH and CANL with power off, console plugged
  in or not.
- The cable is therefore a **branch (stub)** off the bus, and a stub has to stay short: keep it as short as the dash
  allows, ideally **under 30 cm at 500 kbit/s**. CANH/CANL as a twisted pair; 22–24 AWG is fine for the power pair.
  If the console has to sit farther away (roughly beyond 1 m), run the whole bus at 250 kbit/s
  (`ELC_CAN_BITRATE_KBPS=250` on every node), which about doubles the branch length it tolerates.
- **Power:** from the DigiDash +5 V rail (LM2576T-5, 3 A, shared with the three displays and the accessories). Budget about
  300 mA for the console (ESP32-S3 with Wi-Fi off + OLED at full white); the console makes its own 3.3 V.
- Node `0x13`, node type 2, message range `0x4C0`–`0x4FF` (the fourth accessory range). Its messages are defined when its
  firmware is written.
- The console takes its readings (speed, rpm, fuel, temperature, oil, …) from the SENSOR channel of
  [`can_channels.md`](can_channels.md) (center, node `0x01`), not from `0x100`–`0x104`.

## Behaviour

- Frames are sent **single-shot** (no hardware retransmit). Every message repeats, so a lost frame is replaced by the
  next one, and a node with no transceiver (bench) or alone on the bus does not keep retransmitting.
- Receivers accept everything and ignore IDs they do not know. Adding a message never needs the other boards reflashed.
- A controller that goes bus-off is recovered automatically (retry every second).
- `can` on any display's serial console prints counters and the nodes heard.

## Messages

All multi-byte fields are little-endian.

| ID | Sender | Rate | Payload |
| --- | --- | --- | --- |
| `0x100` | center | each sensor frame (20 Hz) | wheel RPM u32, engine RPM u32 |
| `0x101` | center | 20 Hz | fuel ADC u16, engine temp ADC u16, oil ADC u16, charge band u16 |
| `0x102` | center | 20 Hz | ambient ADC u16, flags u8, fault word u16, sequence u8 (6 bytes) |
| `0x103` | center | 20 Hz | odometer tenths u32 (`0xFFFFFFFF` unknown), distance to empty tenths u32 |
| `0x104` | center | 20 Hz | instant MPG ×10 u16, trip MPG ×10 u16, bit0 = has fuel economy (5 bytes) |
| `0x210` | center | on change + every 5 s | cluster state: theme u8 (0 Classic, 1 Fallout, 2 Modern), center sub-view u8, interface screen u8, media screen u8, brightness % u8 (10–100), flags u8 (see below) |
| `0x211` | a display / console | on demand | album art request: slot u8, art id u8, first chunk u16, variant u8 (see below) |
| `0x212` | a display / console | on demand | setting request: setting u8, value u8, from node u8 (see below) |
| `0x213` | center | 1 Hz | center status: flags u8, reserved u8 (was Wi-Fi RSSI), reserved u8 (was the `0x214` serial); reserved bytes are sent as 0 |
| `0x214` | — | — | reserved (was the Wi-Fi network name); nothing sends it |
| `0x215` | console (or any node) | on demand | phone command to an audio bridge: slot u8, command u8, arg u8, from node u8, list serial u8 (see *Phone*) |
| `0x216` | console | on input | menu input for the interface screen: event u8, value s8, from node u8 (see *Console*) |
| `0x217` | interface screen | on change + 1 s while open | menu state: open u8, screen u8, page u8 (0 home, 2 brightness, 3 theme, 4 screens, 5 service, 6 phone; 1 reserved, was Wi-Fi, and ignored), opened by u8 (0 touch, 1 console), text serial u8 |
| `0x218` / `0x219` | interface screen | with `0x217` | menu texts for the console: the focused item or page / its value or state, 5 frames of `[serial << 4 \| chunk]` + 7 bytes each |
| `0x220` | center | 1 Hz | time: UTC s u32, local offset min s16, flags u8 (bit0 valid, bit1 GPS fix, bit2 reserved, was network time), reserved u8 (was the `0x221` serial, sent as 0) |
| `0x221` | — | — | reserved (was the street name); nothing sends it |
| `0x222`–`0x226` | GPS card (node from its slot) | 5 Hz / 1 Hz | position, motion, quality, time (sent on the PPS edge), status: layouts in [`gps_card.md`](../hardware/gps_card.md#can-messages) |
| `0x400 + 0x40·slot + msg` | accessory (slot = node − `0x10`) | accessory-specific | see below |
| `0x700 + node` | every node | 1 Hz | node type u8, firmware version u16, uptime s u32, flags u8 (bit0 = speaks the channel protocol, `can_channels.md`; 0 today) |

Sensor fields have the same meaning as in `sensor_uart.md`. `0x100`–`0x104` are for the three screens; other nodes read the converted readings on the SENSOR channel (`can_channels.md`). Node types: 0 display, 1 audio bridge, 2 console.

### Cluster settings

The center owns the cluster settings (NVS, copied to `settings.json` on the SD card) and sends them in `0x210`.
Screens are 0 left (speedometer), 1 center, 2 right.

- **Interface screen**: the one with the on-screen menu. A tap there opens it (the console's knob will drive it
  too); on the other screens a tap does nothing, and a menu left open closes when the setting moves away.
- **Media screen**: the one with the media view. On the center, media is one of its views (menu or 3 s hold).
  On a side screen it is an extra view next to that screen's gauge, which stays the default: a 3 s hold toggles
  it, and `0x210` flag bit1 says it is showing. Only the screen showing media fetches album art.
- **Brightness**: every screen applies the center's level; bit0 = dims with the headlights.

`0x210` flags: bit0 dims with the headlights, bit1 media showing on the media screen, bit2 the right screen's second
Classic layout (fuel + lamps). Older 2-byte frames still
decode (center, center, 100 %, dims).

Any node changes a setting with `0x212`; the center applies it, saves it and answers with a fresh `0x210` (even
when nothing changed, so the asker sees the outcome). Settings:

| # | Setting | Value |
| --- | --- | --- |
| 1 | theme | 0–2 |
| 2 | center view | 0 tach, 1 clock, 2 media, 3 efficiency (media on a side screen shows it there instead) |
| 3 | interface screen | 0–2 |
| 4 | media screen | 0–2 (media shown here stays shown there) |
| 5 | brightness | 10–100 |
| 6 | dims with the headlights | 0 / 1 |
| 7 | media showing on the media screen | 0 / 1 |
| 8 | demo drive cycle | 0 / 1 |
| 9 | reserved (was: join the saved Wi-Fi network); ignored | — |
| 10 | action: open a center-only page on the center | 2 sensor log, 3 GPS data, 4 phone (1 reserved, was Wi-Fi; ignored) |
| 11 | right screen layout (Classic) | 0 four gauges, 1 fuel + lamps; the right screen follows `0x210` bit2 |

`0x213` flags: bit0 reserved (was Wi-Fi connected), bit1 GPS fix, bit2 NMEA arriving, bit3 audio bridge heard,
bit4 demo on, bit5 and bit6 reserved (were saved Wi-Fi network, joining), bit7 in reverse (the center shows the rear
camera). Reserved bits are sent as 0. The side screens build their menu's Service page from these. The cluster
never uses Wi-Fi.

The center's serial console sets the screens with `menu on left|center|right` and `media on left|center|right`.

Bus load is about 5 % (five 8-byte frames every 50 ms plus heartbeats and audio).

### Audio bridge (node type 1)

| Msg | ID (shown for node `0x10`; add `0x40·(node − 0x10)`: a rev 2 audio card in slot 1 is node `0x14`, `0x500`) | Rate | Payload |
| --- | --- | --- | --- |
| `0x00` transport | `0x400` | 4 Hz | elapsed s u16, duration s u16, volume 0–15 u8, text serial u8 (0–15), flags u8 (bit0 phone connected, bit1 playing: the phone's AVRCP play status), album art id u8 (0 = none ready) |
| `0x01` text | `0x401` | on change + every 1 s | 5 frames: `[serial << 4 \| chunk 0–4]` + 7 bytes of "Artist - Title" (32 bytes max, zero padded) |
| `0x02` art info | `0x402` | answers `0x211` | art id u8, size bytes u32 (0 = no such variant), variant u8 (6 bytes) |
| `0x03` art data | `0x403` | after art info | art id u8, variant << 14 \| chunk u16, up to 5 bytes of image (chunk n = bytes 5n…5n+4) |
| `0x04` phone status | `0x404` | on change + every 1 s | flags u8, call state u8, call s u16, signal \| battery << 4, caller serial \| list serial << 4, paired count \| connected index << 4, call volume 0–15 (see *Phone*) |
| `0x05` phone text | `0x405` | see *Phone* | kind u8, `[serial << 4 \| chunk 0–5]`, 6 bytes of text (32 bytes max, zero padded) |
| `0x06` phone pairing | `0x406` | 2 Hz while pairing | passkey u32 (the code to compare, valid with flag bit5), pairing seconds left u8 |

The text serial bumps whenever the text changes; a receiver shows a text only once all five chunks of the serial named
in the latest transport frame have arrived.

The center takes now-playing, transport and album art from CAN only; neither the center nor the bridge has any I2C
audio code. On the bench the bridge reaches the center through the USB CAN link ([`usb_link.md`](usb_link.md)).

**Album art.** The transport frame names the cover that is ready (the phone's thumbnail, a JPEG of ~5–30 KB). The
bridge makes two smaller copies of each cover before it announces it (`art_variants.cpp`, JPEGDEC + JPEGENC, tens of
ms), so there are three variants:

| Variant | What | Typical size | Time on the bus |
| --- | --- | --- | --- |
| 0 original | the phone's JPEG, as received (usually 200×200) | 8–20 KB | 1.3–3.2 s |
| 1 small | 128×128 JPEG, centre-cropped (64×64 from a bridge without PSRAM) | 2–5 KB (1.5–2.5 KB) | 0.3–0.8 s (0.1–0.4 s) |
| 2 grey 48 | 48×48, 4 bits per pixel, row-major, left pixel in the high nibble (1152 bytes) | 1.1 KB | ~0.2 s |

A receiver sends `0x211` (slot, art id, first chunk 0, variant). The bridge answers with art info (size; 0 means it
has no such variant) and then streams that variant's chunks to the end. The variant rides in the top two bits of the
chunk number, so several streams can run at once without mixing (the center's and the console's). All art streams
together are paced to one frame per 0.8 ms (about a third of the bus), smallest variant first: art IDs lose to the
sensor and cluster frames anyway, but they outrank heartbeats, the other accessories and the console. Frames are
single-shot, so a receiver keeps a bitmap and, once its stream has been quiet for 300 ms, asks again from its first
missing chunk (it gives up on that variant after 20 requests that bring nothing). Receivers accept images up to
48 KB (`ELC_CAN_ART_MAX_BYTES`).

The **media screen** (the center by default) fetches in two steps: the small copy first (shown as soon as it decodes), then the original, which
replaces it. If the small copy is not available it goes straight to the original; if the original fails, the small
picture stays. The **console** asks only for the 48×48 grey image, which its OLED shows as is.

### Phone

The bridge is also the car's hands-free unit (HFP 1.7 hands-free role, wideband speech): call audio plays through
its DACs and the microphone comes in through its ADC. It keeps up to 8 paired phones (most recently used first) and
connects one at a time. Calls are answered and ended on **the console** (soft keys, see *Console* below);
pairing and the paired list are on the round-screen menu's **Phone** page. Codecs: `elc_can_codec.h`, `ElcCanPhone*`.

**Phone status (`0x04`).** Flags byte:

| Bit | Meaning |
| --- | --- |
| 0 | a phone is connected (any profile) |
| 1 | hands-free link up (calls possible) |
| 2 | call audio in the car (SCO link up) |
| 3 | microphone muted |
| 4 | pairing window open (discoverable) |
| 5 | a phone waits for its code to be confirmed (the code is in `0x06`, its name in text kind 2) |
| 6 | the phone plays its own ringtone through the car (in-band ring) |
| 7 | voice assistant active |

Call state: 0 idle, 1 incoming (ringing), 2 dialing, 3 alerting (the other end rings), 4 active, 5 active with a
second call ringing (call waiting), 6 a call on hold. Call seconds count from the answer. Signal and battery are
0–5 as the phone reports them, 15 = unknown. The connected index is the connected phone's place in the paired list
(15 = none). Call volume is the speaker gain, 0–15.

**Phone text (`0x05`).** One ID carries several texts, told apart by the kind byte; each is 6 frames of 6 bytes.
A receiver keeps one decoder per kind it shows (`ElcCanPhoneTextDecoder`).

| Kind | Text | Serial | Sent |
| --- | --- | --- | --- |
| 0 | the call the state is about, as the phone gives its number (no phone book on the bridge): in call waiting (5) the new, ringing call; on hold (6) the active call if there is one, else the held one | the caller serial in `0x04` | on change and every 1 s while a call exists |
| 1 | the connected phone's name | own | every 2 s while connected |
| 2 | the phone asking to pair | own | every 1 s while bit5 is set |
| `0x10` + i | paired phone i (i < 8) | the list serial in `0x04` | all of them when the list changes, then one every 0.5 s in turn |

A receiver shows the caller only when the decoded serial matches the caller serial of the latest status, and a
paired name only when its serial matches the list serial (so a name from before a *Forget* is never shown).

**Phone pairing (`0x06`).** Sent twice a second while the window is open or a code waits. Phones pair with
**numeric comparison**: the phone shows a six-digit code, the console and the displays show the same code, and the
user confirms on the menu's Phone page (interface screen, by touch or with the console's knob). The window opens on command for 2 minutes, and with no time limit (seconds left = 255)
while nothing is paired: from boot, or after the last phone is forgotten. It closes when a phone pairs, on command
(Stop), or when the time runs out. A phone that is not paired cannot pair outside the window; a paired phone can.

**Phone command (`0x215`).** Any node can send it: the console for calls and media, the interface screen's Phone page for pairing and the paired list. Slot = the bridge's message-range index (node − `0x10`: 4 for the audio card in card slot 1), learned from its heartbeat and frames; a receiver accepts a range's audio frames only while that node's heartbeat says audio bridge (node type 1),
from node = the sender. Commands that name a paired index carry the list serial they were read with, and the bridge
ignores them if the list changed since. The bridge answers every command with a fresh `0x04`.

| # | Command | Arg |
| --- | --- | --- |
| 1 | answer | — |
| 2 | end: decline a ringing call, hang up, or cancel dialing | — |
| 3 | microphone mute | 1 mute, 0 unmute |
| 4 | call volume | 0–15 |
| 5 | pairing window | 1 open (2 min), 0 close |
| 6 | confirm the pairing code | 1 accept, 0 reject |
| 7 | connect a paired phone | index |
| 8 | disconnect the connected phone | — |
| 9 | forget a paired phone (unpair) | index |
| 10 | voice assistant | 1 start, 0 stop |
| 11 | redial the last number | — |
| 12 | call waiting | 0 decline the waiting call, 1 end the active call and answer, 2 hold the active call and answer (or swap) |
| 13 | call audio | 1 in the car, 0 on the phone |
| 14 | media | 0 play / pause (from the phone's play status), 1 next track, 2 previous track, 3 play, 4 pause. The console sends 4 when the source leaves Bluetooth and 3 when it comes back |
| 15 | media volume | 0–15: the bridge's **master volume** (it scales its own output, kept across restarts). The phone's own volume is held at full: the bridge puts it back when the phone moves it, so the phone's slider does nothing |

**Displays.** The media screen (`0x210`) shows a call card over its face while a call exists or a pairing code
waits: the caller and the state (incoming, calling, the call time, on hold, muted), or "Pair with …" and the code.
The card names what the console's two keys do; calls are answered and ended there. Pairing and the
paired list are on the menu's **Phone** page (interface screen).

### Console

The console (node `0x13`, `firmware/esp32s3_console`) is a 256×64 amber OLED in 16 grey levels with two knobs
(**Select**, **Volume**, each turns and pushes) and two keys under the OLED (**Back** left, **View** right). Its type
follows the cluster theme (Barlow / Saira / VT323). Design reference: the *El Camino Cluster* prototype artifact.

| Input | Drive pages | OLED menu | Round-screen menu open |
| --- | --- | --- | --- |
| Select turn | next / previous page | move, or change the value being edited (live) | moves it (`0x216` rotate) |
| Select push | open the OLED menu | edit / confirm / open | opens the item (`0x216` press) |
| Back | Now Playing | cancel the edit (the old value comes back), close | back a page, then close (`0x216` back) |
| Volume turn | media volume (call volume in a call), with a bar over whatever shows | | |
| Volume push | play / pause · hold: next track | | |
| View | next center view (`0x212` setting 2) | | |

- **Drive pages** (dots on the right edge): *Media* (48 px grey cover, title, artist, progress, play/pause), *Trip*
  (Trip A, average MPG, range), *Engine* (oil, coolant, fuel, charge with bars, warnings blink), *Clock* (time, date,
  GPS fix or boot clock). Trip A is kept on the console (odometer at reset, in NVS).
- **OLED menu**: Center view, Theme, Brightness, Dim with lights, Menu on, Media on, Right layout, Trip A (reset,
  push again to confirm), OLED contrast (console only), Service (opens that page on the interface screen).
  Settings go to the center with `0x212`.
- **Warnings take over** until acknowledged (push or Back), then blink as a badge: no sensor data, low oil pressure,
  engine hot, not charging, low fuel, odometer not stored. **Reverse** (`0x213` bit7) shows R and the speed.
- **Calls take over** like a warning, and only here do the keys become soft keys, labelled at the bottom of the OLED:
  ringing = **Decline** (Back) / **Answer** (View); call waiting = **Ignore** / **Answer** (holds the active call);
  calling = **End**; active = **End** / **Mute**; on hold = **End** / **Swap**. Everything else about phones
  (pairing, the paired list) is on the menu's Phone page.
- A change made elsewhere shows as a toast (theme, view, menu on, media on). Without CAN for 1 s it shows the bowtie.

## Reserved

| Range | Use |
| --- | --- |
| `0x000`–`0x0FF`, `0x120`–`0x13F`, `0x240`–`0x3FF` | channels of the accessory protocol ([`can_channels.md`](can_channels.md) has the full map); do not use for anything else |
| `0x227`–`0x22F` | more GPS card data, later |
| `0x4C0`–`0x4FF` | console (OLED + knobs board), unused so far |
| `0x500`–`0x6FF` | future; planned for accessory nodes `0x14`–`0x1B` (`docs/hardware/accessory_ports.md`) |
