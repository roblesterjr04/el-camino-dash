# CAN bus

The DigiDash board links the three displays and up to three ESP32 accessory cards on one CAN bus.
Shared code: `firmware/elc_can` (IDs in `elc_can_ids.h`, codecs in `elc_can_codec.h`, driver in `elc_can.h`).
Host tests: `firmware/elc_can/test/host/run_tests.sh`.

The sensor UART fan-out (`sensor_uart.md`) is unchanged: the gauges never depend on CAN. The center re-sends
each sensor frame onto CAN so accessory cards get the same data.

## Physical layer

- 500 kbit/s, 11-bit IDs, one **MCP2562-E/P** (DIP-8) per node: VDD = 5 V, Vio = 3.3 V (board 3.3 V), STBY tied low.
- 120 Ω at the two ends only: **R35 at the left screen (U5)** and **R36 at the right screen (U10)**. Bus order:
  U5 left → U6 slot 0 → U7 slot 1 → U8 slot 2 → U9 center → U10 right.
- Each TXD has a 10 kΩ pull-up to 3.3 V, so a node that is booting (or a slot that is empty) holds the bus recessive.
  Slot RXD lines go through 1 kΩ so an unpowered DevKit is not fed through its RX pin.
- **J16** (CANH, CANL, GND) is a tap for a USB-CAN adapter.

| Node | Board part | CAN TX | CAN RX | Node number |
| --- | --- | --- | --- | --- |
| Left screen (speedo) | J5 → U5 | GPIO34 (header pin 31) | GPIO36 (header pin 23) | `0x02` |
| Center screen | J1 → U9 | GPIO34 (header pin 31) | GPIO36 (header pin 23) | `0x01` |
| Right screen (TSPS) | J4 → U10 | GPIO34 (header pin 31) | GPIO36 (header pin 23) | `0x03` |
| Slot 0 (U3, audio bridge) | U6 | GPIO32 | GPIO33 | `0x10` |
| Slot 1 (U11) | U7 | GPIO32 | GPIO33 | `0x11` |
| Slot 2 (U12) | U8 | GPIO32 | GPIO33 | `0x12` |

GPIO34/36 are P4 strapping pins but safe here: boot mode is set by GPIO35 (left pulled up), GPIO36 only
matters when GPIO35 is low, and the TXD pull-up holds GPIO34 high during reset.

Build flags: `ELC_CAN_TX_PIN` / `ELC_CAN_RX_PIN` (set in each `platformio.ini`; `-1` turns CAN off).

### Slot ID straps

A card finds its slot by reading GPIO13 (ID0) and GPIO23 (ID1) with pull-ups; a pin grounded by the board reads as 1.

| Slot | GPIO13 | GPIO23 |
| --- | --- | --- |
| 0 (U3) | open | open |
| 1 (U11) | GND | open |
| 2 (U12) | open | GND |

Slot cards get 5 V and GND from the board and run from their own 3.3 V regulator (the DevKit 3V3 pin is not connected).

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
| `0x400 + 0x40·slot + msg` | slot card | card-specific | see below |
| `0x700 + node` | every node | 1 Hz | node type u8, firmware version u16, uptime s u32, flags u8 |

Sensor fields have the same meaning as in `sensor_uart.md`. Node types: 0 display, 1 audio bridge.

Bus load is about 5 % (five 8-byte frames every 50 ms plus heartbeats and audio).

### Audio bridge (node type 1)

| Msg | ID (slot 0) | Rate | Payload |
| --- | --- | --- | --- |
| `0x00` transport | `0x400` | 4 Hz | elapsed s u16, duration s u16, volume 0–15 u8, text serial u8 (0–15), bit0 = phone connected (7 bytes) |
| `0x01` text | `0x401` | on change + every 1 s | 5 frames: `[serial << 4 \| chunk 0–4]` + 7 bytes of "Artist - Title" (32 bytes max, zero padded) |

The text serial bumps whenever the text changes; a receiver shows a text only once all five chunks of the serial named
in the latest transport frame have arrived.

The center takes now-playing from CAN when it hears it (within 2 s) and falls back to the I2C link otherwise
(`cluster_i2c.md`). Album art is still read over I2C only.

## Reserved

| Range | Use |
| --- | --- |
| `0x220`–`0x22F` | GPS time / position (not sent yet) |
| `0x4C0`–`0x6FF` | future |
