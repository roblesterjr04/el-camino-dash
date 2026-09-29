# CAN bus

The DigiDash board links the three displays, up to three ESP32 accessory cards and the console (OLED + knobs, on its
own board at the end of a short cable) on one CAN bus. **CAN is the link in the car** for everything except the sensor UART:
the I2C audio link (`cluster_i2c.md`) exists for bench setups only and is compiled in only by the `*-bench` / `_bench`
PlatformIO envs.
Shared code: `firmware/elc_can` (IDs in `elc_can_ids.h`, codecs in `elc_can_codec.h`, driver in `elc_can.h`).
Host tests: `firmware/elc_can/test/host/run_tests.sh`.

The sensor UART fan-out (`sensor_uart.md`) is unchanged: the gauges never depend on CAN. The center re-sends
each sensor frame onto CAN so accessory cards get the same data.

## Physical layer

- 500 kbit/s, 11-bit IDs, one **MCP2562-E/P** (DIP-8) per node: VDD = 5 V, Vio = 3.3 V (board 3.3 V), STBY tied low.
- 120 Ω at the two ends only: **R35 at the left screen (U5)** and **R36 at the right screen (U10)**. Bus order:
  U5 left → U6 slot 0 → U7 slot 1 → U8 slot 2 → U9 center → U10 right. The console hangs off J17 as an
  unterminated branch (below); the bus is complete and terminated with or without it.
- Each TXD has a 10 kΩ pull-up to 3.3 V, so a node that is booting (or a slot that is empty) holds the bus recessive.
  Slot RXD lines go through 1 kΩ so an unpowered DevKit is not fed through its RX pin.
- **J16** (CANH, CANL, GND) is a tap for a USB-CAN adapter; **J17** next to it is the console cable (below).

| Node | Board part | CAN TX | CAN RX | Node number |
| --- | --- | --- | --- | --- |
| Left screen (speedo) | J5 → U5 | GPIO34 (header pin 31) | GPIO36 (header pin 23) | `0x02` |
| Center screen | J1 → U9 | GPIO34 (header pin 31) | GPIO36 (header pin 23) | `0x01` |
| Right screen (TSPS) | J4 → U10 | GPIO34 (header pin 31) | GPIO36 (header pin 23) | `0x03` |
| Slot 0 (U3, audio bridge) | U6 | GPIO32 | GPIO33 | `0x10` |
| Slot 1 (U11) | U7 | GPIO32 | GPIO33 | `0x11` |
| Slot 2 (U12) | U8 | GPIO32 | GPIO33 | `0x12` |
| Console (own board, cable to J17) | on the console | its DevKit's pins | | `0x04` |

GPIO34/36 are P4 strapping pins but safe here: boot mode is set by GPIO35 (left pulled up), GPIO36 only
matters when GPIO35 is low, and the TXD pull-up holds GPIO34 high during reset.

Build flags: `ELC_CAN_TX_PIN` / `ELC_CAN_RX_PIN` (set in each `platformio.ini`; `-1` turns CAN off).

### Console cable (J17)

The console is its own board: an ESP32-DevKitC-32 (the same module as the slots), one MCP2562, the SSD1322 256×64
OLED on SPI, the Select and Volume encoders and the Back and View keys. One cable runs from it to **J17** on the
DigiDash board, a JST PH 4-pin vertical header (B4B-PH-K) next to the J16 debug tap:

| J17 pin | Signal |
| --- | --- |
| 1 | +5 V (from the DigiDash rail, via C23 at U10) |
| 2 | GND |
| 3 | CANL |
| 4 | CANH |

- **Termination stays on the DigiDash board** (R35 at U5, R36 at U10), so the bus works the same with the console
  unplugged. **The console board has no 120 Ω.** Check: ~60 Ω between CANH and CANL with power off, console plugged
  in or not.
- The cable is therefore a **branch (stub)** off the bus, and a stub has to stay short: keep it as short as the dash
  allows, ideally **under 30 cm at 500 kbit/s**. CANH/CANL as a twisted pair; 22–24 AWG is fine for the power pair.
  If the console has to sit farther away (roughly beyond 1 m), run the whole bus at 250 kbit/s
  (`ELC_CAN_BITRATE_KBPS=250` on every node), which about doubles the branch length it tolerates.
- **Power:** from the DigiDash +5 V rail (LM2576T-5, 3 A, shared with the three displays and the slots). Budget about
  300 mA for the console (ESP32 with Wi-Fi off + OLED at full white); the console makes its own 3.3 V.
- Node `0x04`, node type 2, message range `0x4C0`–`0x4FF` (after the three slots). Its messages are defined when its
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
| `0x210` | center | on change + every 5 s | theme u8 (0 Classic, 1 Fallout, 2 Modern), center sub-view u8 |
| `0x211` | a display | on demand | album art request: slot u8, art id u8, first chunk u16 (see below) |
| `0x400 + 0x40·slot + msg` | slot card | card-specific | see below |
| `0x700 + node` | every node | 1 Hz | node type u8, firmware version u16, uptime s u32, flags u8 |

Sensor fields have the same meaning as in `sensor_uart.md`. Node types: 0 display, 1 audio bridge, 2 console.

Bus load is about 5 % (five 8-byte frames every 50 ms plus heartbeats and audio).

### Audio bridge (node type 1)

| Msg | ID (slot 0) | Rate | Payload |
| --- | --- | --- | --- |
| `0x00` transport | `0x400` | 4 Hz | elapsed s u16, duration s u16, volume 0–15 u8, text serial u8 (0–15), bit0 = phone connected, album art id u8 (0 = none ready) |
| `0x01` text | `0x401` | on change + every 1 s | 5 frames: `[serial << 4 \| chunk 0–4]` + 7 bytes of "Artist - Title" (32 bytes max, zero padded) |
| `0x02` art info | `0x402` | answers `0x211` | art id u8, size bytes u32 (5 bytes) |
| `0x03` art data | `0x403` | after art info | art id u8, chunk u16, up to 5 bytes of JPEG (chunk n = bytes 5n…5n+4) |

The text serial bumps whenever the text changes; a receiver shows a text only once all five chunks of the serial named
in the latest transport frame have arrived.

The center takes now-playing, transport and album art from CAN only. A bench build of the center
(`CENTER_AUDIO_I2C=1`, the `*-bench` envs) also reads a bridge wired to its I2C bus (`cluster_i2c.md`) when none is
heard on CAN; a bench build of the bridge (`esp32dev_bench`) starts its I2C slaves. The default builds have no I2C
audio code at all.

**Album art.** The transport frame names the art id that is ready (the phone's cover thumbnail, a JPEG of ~5–30 KB).
A display that wants it sends `0x211` (slot, art id, first chunk 0). The bridge answers with art info (size), then
streams art data chunks from that chunk to the end. The stream is paced to one frame per 0.8 ms (about a third of
the bus): art IDs lose to the sensor and cluster frames anyway, but they outrank heartbeats and the other slots and
the console. A 20 KB cover takes about 3 s. Frames are single-shot, so the receiver keeps a bitmap and, once the
stream has been quiet for 300 ms, sends `0x211` again from its first missing chunk (it gives up on that art id after
20 requests that bring nothing). Receivers accept images up to 48 KB (`ELC_CAN_ART_MAX_BYTES`). Any display (or the
console) can collect a stream another node asked for.

## Reserved

| Range | Use |
| --- | --- |
| `0x220`–`0x22F` | GPS time / position (not sent yet) |
| `0x4C0`–`0x4FF` | console (OLED + knobs board), messages to be defined |
| `0x500`–`0x6FF` | future |
