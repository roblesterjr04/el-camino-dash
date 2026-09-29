# CAN bus

The DigiDash board links the three displays, accessory carrier boards (on its accessory ports) and the console
(OLED + knobs, on its own board at the end of a short cable) on one CAN bus. **CAN is the link in the car** for
everything except the sensor UART: the I2C audio link (`cluster_i2c.md`) exists for bench setups only and is compiled
in only by the `*-bench` / `_bench` PlatformIO envs. Ports, carriers and their rules:
[`docs/hardware/accessory_ports.md`](../hardware/accessory_ports.md).

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
on the DigiDash board, a locking JST GH 4-pin (BM04B-GHS-TBT) with the DroneCAN / Pixhawk pinout (J22 is a second,
spare cable port):

| GH pin | Signal |
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
| `0x213` | center | 1 Hz | center status: flags u8, Wi-Fi RSSI s8, text serial u8 |
| `0x214` | center | on change + every 5 s | Wi-Fi network name: 5 frames of `[serial << 4 \| chunk]` + 7 bytes (connected network, else the saved one) |
| `0x400 + 0x40·slot + msg` | accessory (slot = node − `0x10`) | accessory-specific | see below |
| `0x700 + node` | every node | 1 Hz | node type u8, firmware version u16, uptime s u32, flags u8 |

Sensor fields have the same meaning as in `sensor_uart.md`. Node types: 0 display, 1 audio bridge, 2 console.

### Cluster settings

The center owns the cluster settings (NVS, copied to `settings.json` on the SD card) and sends them in `0x210`.
Screens are 0 left (speedometer), 1 center, 2 right.

- **Interface screen**: the one with the on-screen menu. A tap there opens it (the console's knob will drive it
  too); on the other screens a tap does nothing, and a menu left open closes when the setting moves away.
- **Media screen**: the one with the media view. On the center, media is one of its views (menu or 3 s hold).
  On a side screen it is an extra view next to that screen's gauge, which stays the default: a 3 s hold toggles
  it, and `0x210` flag bit1 says it is showing. Only the screen showing media fetches album art.
- **Brightness**: every screen applies the center's level; bit0 = dims with the headlights.

`0x210` flags: bit0 dims with the headlights, bit1 media showing on the media screen. Older 2-byte frames still
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
| 9 | action: join the saved Wi-Fi network | — |
| 10 | action: open a center-only page on the center | 1 Wi-Fi scan and passwords, 2 sensor log, 3 GPS data, 4 phone |

`0x213` flags: bit0 Wi-Fi connected, bit1 GPS fix, bit2 NMEA arriving, bit3 audio bridge heard, bit4 demo on,
bit5 a saved network exists, bit6 joining a network. The text serial names the `0x214` text that goes with it.
The side screens build their menu's Wi-Fi and Service pages from these.

The center's serial console sets the screens with `menu on left|center|right` and `media on left|center|right`.

Bus load is about 5 % (five 8-byte frames every 50 ms plus heartbeats and audio).

### Audio bridge (node type 1)

| Msg | ID (node `0x10`) | Rate | Payload |
| --- | --- | --- | --- |
| `0x00` transport | `0x400` | 4 Hz | elapsed s u16, duration s u16, volume 0–15 u8, text serial u8 (0–15), bit0 = phone connected, album art id u8 (0 = none ready) |
| `0x01` text | `0x401` | on change + every 1 s | 5 frames: `[serial << 4 \| chunk 0–4]` + 7 bytes of "Artist - Title" (32 bytes max, zero padded) |
| `0x02` art info | `0x402` | answers `0x211` | art id u8, size bytes u32 (0 = no such variant), variant u8 (6 bytes) |
| `0x03` art data | `0x403` | after art info | art id u8, variant << 14 \| chunk u16, up to 5 bytes of image (chunk n = bytes 5n…5n+4) |

The text serial bumps whenever the text changes; a receiver shows a text only once all five chunks of the serial named
in the latest transport frame have arrived.

The center takes now-playing, transport and album art from CAN only. A bench build of the center
(`CENTER_AUDIO_I2C=1`, the `*-bench` envs) also reads a bridge wired to its I2C bus (`cluster_i2c.md`) when none is
heard on CAN; a bench build of the bridge (`esp32dev_bench`) starts its I2C slaves. The default builds have no I2C
audio code at all.

**Album art.** The transport frame names the cover that is ready (the phone's thumbnail, a JPEG of ~5–30 KB). The
bridge makes two smaller copies of each cover before it announces it (`art_variants.cpp`, JPEGDEC + JPEGENC, tens of
ms), so there are three variants:

| Variant | What | Typical size | Time on the bus |
| --- | --- | --- | --- |
| 0 original | the phone's JPEG, as received (usually 200×200) | 8–20 KB | 1.3–3.2 s |
| 1 128 px | 128×128 JPEG, centre-cropped | 2–5 KB | 0.3–0.8 s |
| 2 grey 48 | 48×48, 4 bits per pixel, row-major, left pixel in the high nibble (1152 bytes) | 1.1 KB | ~0.2 s |

A receiver sends `0x211` (slot, art id, first chunk 0, variant). The bridge answers with art info (size; 0 means it
has no such variant) and then streams that variant's chunks to the end. The variant rides in the top two bits of the
chunk number, so several streams can run at once without mixing (the center's and the console's). All art streams
together are paced to one frame per 0.8 ms (about a third of the bus), smallest variant first: art IDs lose to the
sensor and cluster frames anyway, but they outrank heartbeats, the other accessories and the console. Frames are
single-shot, so a receiver keeps a bitmap and, once its stream has been quiet for 300 ms, asks again from its first
missing chunk (it gives up on that variant after 20 requests that bring nothing). Receivers accept images up to
48 KB (`ELC_CAN_ART_MAX_BYTES`).

The **media screen** (the center by default) fetches in two steps: the 128 px copy first (shown as soon as it decodes), then the original, which
replaces it. If the 128 px copy is not available it goes straight to the original; if the original fails, the 128 px
picture stays. The **console** asks only for the 48×48 grey image, which its OLED shows as is.

## Reserved

| Range | Use |
| --- | --- |
| `0x220`–`0x22F` | GPS time / position (not sent yet) |
| `0x4C0`–`0x4FF` | console (OLED + knobs board), messages to be defined |
| `0x500`–`0x6FF` | future; planned for accessory nodes `0x14`–`0x1B` (`docs/hardware/accessory_ports.md`) |
