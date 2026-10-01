# GPS card

**Status: spec only.** No schematic or board yet. When it is drawn, it follows the audio card's generator pattern
(`AudioCarrier/generate/`).

A slot card for the DigiDash rev 2 board (`accessory_ports.md`, rev 2 section) that reads a **remote GPS puck** (the
receiver and its antenna in one housing, on the dash under the windshield) over a 4-pin cable, and sends position,
motion and time on CAN. It has **no radio and no RF parts**: no antenna connector, no antenna bias circuit. It replaces
the GPS on J6, which becomes a plain serial port. The card's GPS connector has the same pinout as J6, so the same puck
and cable work on either.

## What it does

- Reads the puck's UART. With a u-blox puck it configures it for **UBX-NAV-PVT** at 5 Hz: one binary message with
  everything (position, speed, heading, fix, satellites, accuracy, time). With any other puck it parses NMEA (RMC,
  GGA, GSA) at whatever rate the puck sends, usually 1 Hz.
- Sends **position and motion** at the puck's rate (5 Hz with UBX), and **quality, time and status at 1 Hz**, on fixed
  IDs `0x222`–`0x226` (see [CAN messages](#can-messages)). Every node hears them; nobody relays them.
- Hot start after key-off comes from the puck's own backup battery or supercap (most pucks have one). The card does not
  power the puck with the key off.

## Parts

| Part | Choice | Why |
| --- | --- | --- |
| MCU | **STM32G0B1KBU6** (UFQFPN-32, 5 × 5 mm; 64 MHz Cortex-M0+, 128 KB flash, 144 KB RAM) | Built-in CAN controller (FDCAN, runs classic CAN at 500 kbit/s), no radio, cheap. Its UART pins are 5 V tolerant, so a puck with 5 V logic is fine too. Arduino core (STM32duino) or STM32Cube |
| GPS connector | **JST PH 4-pin, side entry (S4B-PH-K)** on the top edge | Same part and pinout as the main board's J6 |
| Puck supply | 3.3 V or 5 V, picked by solder jumper JP1 (3.3 V fitted), through a 0.2 A PTC | 3.3 V matches J6 and NEO-6M / NEO-M8 boards. Many drone-style pucks want 3.6–5.5 V: move JP1 to 5 V |
| Line protection | 100 Ω in series on TX and RX, a low-capacitance ESD diode array on TX, RX and supply | The cable runs up to the windshield |
| CAN transceiver | MCP2562 (VDD 5 V, VIO 3.3 V, STBY low) | As every other node. No termination |
| 3.3 V | AP2112K-3.3 from the slot's 5 V | The MCU, the transceiver's VIO and a 3.3 V puck (most draw 25–50 mA) |
| Clock | 8 MHz crystal (HSE) | CAN needs better timing accuracy than the internal oscillator gives over temperature |
| Programming | SWD: Tag-Connect TC2030-NL pads (or a 2 × 5 1.27 mm header), BOOT0 pulled low | |
| Indicator | One LED: off = nothing from the puck, blinking = searching, steady = 3D fix | |

### GPS connector (J2)

| Pin | Signal |
| --- | --- |
| 1 | Puck power (3.3 V, or 5 V by JP1), 0.2 A PTC |
| 2 | TX (out of the card: to the puck's RX) |
| 3 | RX (into the card: from the puck's TX) |
| 4 | GND |

Pin for pin the same as J6 on the main board. A puck with a different plug gets a short adapter cable.

**Slot pins used:** 1 / 2 (+5 V), GND, 5 / 6 (CAN), 11 (slot ID). Everything else is left unconnected.

## MCU pins

| Function | STM32G0B1 | Notes |
| --- | --- | --- |
| CAN | FDCAN1 RX / TX | To the MCP2562 |
| Puck UART | USART2 TX / RX | Auto-baud at start-up: 9600, 38400, 115200 |
| Slot ID | an ADC input with 10k to 3.3 V | The main board's resistor to GND sets the slot (see `accessory_ports.md`) |
| LED | GPIO | |
| HSE | OSC_IN / OSC_OUT | 8 MHz |
| Debug UART | USART1 on test pads | Log output |

Check the exact pin numbers against the STM32G0B1 datasheet's alternate-function table when the schematic is drawn.

## Board

- **80 × 60 mm** like every card, any slot. It is nearly empty: a few small chips. Parts on the parts side up to 15 mm;
  the back only low SMD parts.
- **Top edge** (the case panel): J2.
- **Bottom edge:** the 2×15 right-angle header, placed exactly as on the audio card (`AudioCarrier/generate/gen_pcb.py`
  has the orientation check), with the MCU, transceiver and regulator near it.
- **Layers:** 2 are enough (no RF, slow signals). 4 only if it is panelised with the other cards.

## Firmware

1. At boot: read the slot ID. Find the puck's baud rate. If it answers UBX, configure it over UBX (UBX-CFG-VALSET on
   M10, UBX-CFG-* on older parts): 115200 baud, 5 Hz, UBX-NAV-PVT on, NMEA off, every constellation it has on. Else use
   NMEA as it comes.
2. Each fix (5 Hz with UBX): send `0x222` and `0x223`.
3. Once a second: send `0x225` as soon as the second's message arrives, then `0x224` and `0x226`.
4. Heartbeat `0x705` at 1 Hz (node type 3, sensor node). Channel protocol (`can_channels.md`): NET ANNOUNCE as product
   `0x0005` with the slot in its flags. A SENSOR description for "GPS speed" is optional.
5. With no fix, or no puck: still send every frame at 1 Hz with fix type 0, so receivers know the card is alive.

**Center firmware change:** the center takes GPS from CAN when it has heard `0x225` within the last 3 s, and falls back
to the UART on J6 otherwise. It keeps sending the time (`0x220`, flags bit1 = GPS) and the street name as today.

## CAN messages

Node `0x05` (a core node: `can_channels.md` reserves `0x04`–`0x0F`). Fixed IDs, little-endian, classic 8-byte frames.

| ID | Rate | Payload |
| --- | --- | --- |
| `0x222` | each fix (5 Hz with UBX, 1 Hz with NMEA) | **Position**: latitude s32 (1e-7°), longitude s32 (1e-7°) |
| `0x223` | with `0x222` | **Motion**: ground speed u16 (cm/s), heading u16 (0.01°, 0 = north), altitude s16 (m above sea level), fix type u8 (0 none, 2 2D, 3 3D), satellites used u8 |
| `0x224` | 1 Hz | **Quality**: horizontal accuracy u16 (cm), vertical accuracy u16 (cm), speed accuracy u16 (cm/s), PDOP u8 (×10, 255 = above 25.5), flags u8 (bit0 fix valid, bit1 time valid, bit2 date valid, bit3 UBX (accuracy fields real; 0xFFFF when NMEA-only)) |
| `0x225` | 1 Hz | **Time**: UTC Unix seconds u32, milliseconds u16 (of the fix the time came from), flags u8 (bit0 valid, bit1 leap seconds known), sequence u8 |
| `0x226` | 1 Hz | **Status**: satellites seen u8, constellations used u8 (bit0 GPS, bit1 Galileo, bit2 GLONASS, bit3 BeiDou, bit4 QZSS, bit5 SBAS), seconds since the last fix u16 (65535 = never), puck u8 (0 none heard, 1 u-blox UBX, 2 NMEA only), firmware version u16, slot u8 |

`0x227`–`0x22F` stay reserved. Bus load is about 13 frames a second, well under 1 %.

- **Time accuracy:** with four pins there is no PPS line, so the time frame lags the true second by the puck's output
  delay plus the UART transfer (tens of milliseconds). Plenty for a clock. If sub-millisecond time is ever needed, a
  puck with a PPS wire and a fifth pin on J2 would add it.
- **Speed vs the speedometer:** GPS speed is the true speed and ignores tyre size. The center can show it next to the
  wheel-speed reading, or use it to calibrate the speed sensor's pulses per mile.
