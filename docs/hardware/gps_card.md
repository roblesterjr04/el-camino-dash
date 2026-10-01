# GPS card

**Status: spec only.** No schematic or board yet. When it is drawn, it follows the audio card's generator pattern
(`AudioCarrier/generate/`).

A slot card for the DigiDash rev 2 board (`accessory_ports.md`, rev 2 section) that reads a GNSS module and sends
position, motion and time on CAN. It has **no radio**. It replaces the GPS module on J6, which becomes a plain serial
port.

## What it does

- Reads a u-blox GNSS module's **UBX-NAV-PVT** message: one binary message with everything (position, speed, heading,
  fix, satellites, accuracy, time). This is cleaner than parsing NMEA sentences.
- Sends **position and motion at 5 Hz**, and **quality, time and status at 1 Hz**, on fixed IDs `0x222`–`0x226`
  (see [CAN messages](#can-messages)). Every node hears them; nobody relays them.
- Keeps the module's backup memory alive from the slot's constant 12 V, so after a key-off the next fix is a **hot
  start** (a second or two, instead of half a minute or more).
- Times its time frame to the module's 1 PPS (pulse-per-second) edge, so a receiver can set its clock to within a
  few milliseconds.

## Parts

| Part | Choice | Why |
| --- | --- | --- |
| MCU | **STM32G0B1KBU6** (UFQFPN-32, 5 × 5 mm; 64 MHz Cortex-M0+, 128 KB flash, 144 KB RAM) | Built-in CAN controller (FDCAN, runs classic CAN at 500 kbit/s), no radio, cheap, plenty of UARTs and timers. Arduino core (STM32duino) or STM32Cube |
| GNSS module | **u-blox MAX-M10S** (LCC, 9.7 × 10.1 mm) | GPS + Galileo + GLONASS + BeiDou at once, low power, UBX protocol, 1 PPS, backup supply pin. Cheaper alternative: Quectel L76K (NMEA only) |
| CAN transceiver | MCP2562 (VDD 5 V, VIO 3.3 V, STBY low) | As every other node. No termination |
| 3.3 V | AP2112K-3.3 from the slot's 5 V | The MCU and module draw about 40 mA. The GNSS module wants its own clean regulator, not the slot's shared 3.3 V |
| Backup supply | MCP1703A-3302 (16 V max in, 2 µA quiescent) from slot pin 10 (constant 12 V), behind 1 kΩ and a 15 V zener (BZT52C15) | Feeds the module's V_BCKP pin with the key off |
| Clock | 8 MHz crystal (HSE) | CAN needs better timing accuracy than the internal oscillator gives over temperature |
| Antenna | SMA female, edge-launch, on the top edge; an **active GPS antenna** (magnetic or stick-on puck, 3–5 V, 28 dB) on the dash under the windshield | The card sits behind the dash under a metal roof, so the antenna has to be remote. Feed it from the module's VCC_RF output through the active-antenna circuit in u-blox's integration manual (inductor bias tee, current limit) |
| Programming | SWD: Tag-Connect TC2030-NL pads (or a 2 × 5 1.27 mm header), BOOT0 pulled low | |
| Indicator | One LED: off = no data from the module, blinking = searching, steady = 3D fix | |

**Slot pins used:** 1 / 2 (+5 V), GND, 5 / 6 (CAN), 10 (constant 12 V), 11 (slot ID). Everything else (switched 12 V,
3.3 V, WAKE, LAMP, REV, the links, the audio bus, AUX 12 V) is left unconnected.

## MCU pins

| Function | STM32G0B1 | Notes |
| --- | --- | --- |
| CAN | FDCAN1 RX / TX | To the MCP2562 |
| GNSS UART | USART2 TX / RX | 38400 baud at power-up (the M10 default). Firmware switches it to 115200 |
| 1 PPS | a timer's input-capture channel (TIM2 CH1) | Timestamps the pulse; the time frame goes out right after it |
| GNSS reset | GPIO, open drain, to the module's RESET_N | Recovery only |
| Slot ID | an ADC input with 10k to 3.3 V | The main board's resistor to GND sets the slot (see `accessory_ports.md`) |
| LED | GPIO | |
| HSE | OSC_IN / OSC_OUT | 8 MHz |
| Debug UART | USART1 on test pads | Log output |

Check the exact pin numbers against the STM32G0B1 datasheet's alternate-function table when the schematic is drawn.

## Board

- **80 × 60 mm** like every card, any slot. Parts on the parts side up to 15 mm (the SMA is the tallest); the back
  only low SMD parts.
- **Top edge** (the case panel): the SMA. The module sits right behind it, so the RF trace is a few millimetres long.
- **Bottom edge:** the 2×15 right-angle header, placed exactly as on the audio card (`AudioCarrier/generate/gen_pcb.py`
  has the orientation check), with the MCU, transceiver and regulators near it.
- **RF:** a 50 Ω coplanar trace with ground on both sides, from the module to the SMA. Width from the fab's impedance
  calculator for its 4-layer stackup. Solid ground under the module and the trace. Keep the crystal, the MCU and the
  CAN transceiver away from the module and its antenna trace.
- **Layers:** 4 (F signal and RF, In1 solid GND, In2 power, B signal), the same rules as the audio card.

## Firmware

1. At boot: read the slot ID; configure the module over UBX (UBX-CFG-VALSET): 115200 baud, 5 Hz navigation rate, UBX-NAV-PVT
   on the UART, NMEA off, all four constellations on.
2. Each NAV-PVT (5 Hz): send `0x222` and `0x223`.
3. Each PPS edge (1 Hz): send `0x225` straight away, then `0x224` and `0x226`.
4. Heartbeat `0x705` at 1 Hz (node type 3, sensor node). Channel protocol (`can_channels.md`): NET ANNOUNCE as product
   `0x0005` with the slot in its flags. A SENSOR description for "GPS speed" is optional.
5. With no fix: still send every frame at 1 Hz with fix type 0, so receivers know the card is alive and searching.

**Center firmware change:** the center takes GPS from CAN when it has heard `0x225` within the last 3 s, and falls back
to the UART on J6 otherwise. It keeps sending the time (`0x220`, flags bit1 = GPS) and the street name as today.

## CAN messages

Node `0x05` (a core node: `can_channels.md` reserves `0x04`–`0x0F`). Fixed IDs, little-endian, classic 8-byte frames.

| ID | Rate | Payload |
| --- | --- | --- |
| `0x222` | 5 Hz | **Position**: latitude s32 (1e-7°), longitude s32 (1e-7°) |
| `0x223` | 5 Hz | **Motion**: ground speed u16 (cm/s), heading u16 (0.01°, 0 = north), altitude s16 (m above sea level), fix type u8 (0 none, 2 2D, 3 3D), satellites used u8 |
| `0x224` | 1 Hz | **Quality**: horizontal accuracy u16 (cm), vertical accuracy u16 (cm), speed accuracy u16 (cm/s), PDOP u8 (×10, 255 = above 25.5), flags u8 (bit0 fix valid, bit1 time valid, bit2 date valid, bit3 hot start, bit4 antenna OK, bit5 antenna fault) |
| `0x225` | 1 Hz, right after the PPS edge | **Time**: UTC Unix seconds u32 (of the PPS edge), milliseconds since the edge u16 when sent, flags u8 (bit0 valid, bit1 leap seconds known), sequence u8 |
| `0x226` | 1 Hz | **Status**: satellites seen u8, constellations used u8 (bit0 GPS, bit1 Galileo, bit2 GLONASS, bit3 BeiDou, bit4 QZSS, bit5 SBAS), seconds since the last fix u16 (65535 = never), module u8 (0 none, 1 MAX-M10S, 2 L76K), firmware version u16, slot u8 |

`0x227`–`0x22F` stay reserved. Bus load is about 13 frames a second, well under 1 %.

- **Speed vs the speedometer:** GPS speed is the true speed and ignores tyre size. The center can show it next to the
  wheel-speed reading, or use it to calibrate the speed sensor's pulses per mile.
- **Antenna fault** (bit5) needs an antenna supervisor in the active-antenna circuit. Without one, bits 4–5 stay 0.
