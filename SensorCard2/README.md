# Sensor card, rev B: STM32G0B1

The DigiDash sensor pack on a slot card, rev A ([`SensorCard/`](../SensorCard/)) with the ATmega2560 + MCP2515 replaced
by the GPS and radio cards' **STM32G0B1KBU6** (Cortex-M0+, built-in FDCAN), on **two layers**. Same 80 × 60 mm card,
same connectors in the same places, same CAN node (`0x04`, fixed), same frames, same FRAM odometer. Started 2026-10-26
to take the one 100-pin, $13 chip and the CAN controller off the BOM and make the card a 2-layer board.

| File | What |
| --- | --- |
| `SensorCard2.kicad_sch` / `.kicad_pcb` / `.kicad_pro` | The card: 80 × 60 mm, 2 layers, all parts on the parts side (F). KiCad 10 |
| `SensorCard2_BOM.csv` | Parts list (`generate/gen_bom.py`) |
| `fab/` | Gerbers, drill, placement files, the assembler's BOM (`python3 ../tools/fab_export.py SensorCard2`) |
| `generate/` | `gen_sch.py`, `gen_pcb.py` (placement + Freerouting, 2 layers), `gen_bom.py`, `drc.py` |

## What changed from rev A

| | Rev A | Rev B |
| --- | --- | --- |
| MCU | ATmega2560-16AU, 5 V, 16 MHz, TQFP-100 | STM32G0B1KBU6, 3.3 V, 64 MHz, UFQFPN-32, 8 MHz HSE from a CMOS oscillator (bypass) |
| CAN | MCP2515 controller (SPI, own crystal) + transceiver | The MCU's FDCAN1 (PA11 / PA12) + TJA1051T/3 (VDD 5 V, VIO 3.3 V) |
| Logic level | 5 V | 3.3 V. The sensor serial out (J8) keeps a 5 V output through a 74LVC1T45, so the main board's 10k / 20k dividers still see rev A's level |
| Senders | Pull-up banks from 5 V, ADC ratiometric to 5 V | Pull-up banks from **3.3 V**, ADC ratiometric to 3.3 V: a resistive sender gives the **same counts** as rev A, so the screens' formulas and the calibration data carry over |
| 3-wire transducers | Up to 5 V into the ADC | The ADC pin clamps at 3.3 V. Each channel has a DIV footprint (not fitted): a 20k there with the 10k series makes a 2/3 divider for a 0.5-4.5 V output |
| Feed monitors | ADC | Digital (feed present) on PB2 / PB9, through 10k / 15k |
| Programming | ISP (2 × 3) | SWD (1 × 5, ST-Link), as the other cards; the STM32 arrives blank |
| Spare I/O header, demo jumper | J10, J9 | Gone: the 32-pin part has no pins to spare. Demo mode becomes a console command |
| Layers | 4 | 2 |

Unchanged: the slot header and its pin use, J2 senders, J3 speed (with JP1 and its PTC), J4 tach (PC817 isolation),
J5 RPM select, J7 (FTDI pinout, now 3.3 V logic: USART1), J8 sensor serial out, the sender banks and their solder
jumpers, the 0.1 A PTC feeds, battery and ignition sense (dividers rescaled for 3.3 V), WAKE in and out.

## Clock: an oscillator, not a crystal (fixed 2026-11-13)

The first rev B boards were drawn with an 8 MHz crystal (YXC X32258MOB4SI) and two 15 pF load capacitors on PC14 /
PC15. **That crystal can never start**: the 32-pin STM32G0B1 has no PF0 / PF1, so it has no HSE crystal driver. ST's
pin data (`MCU_ST_STM32G0` in KiCad, generated from it) gives PC14 `RCC_OSC_IN` and PC15 only `RCC_OSC_EN` /
`RCC_OSC32_OUT`, no `RCC_OSC_OUT`. A clock into PC14 in HSE bypass is the only external HSE. On those boards the
firmware falls back to the HSI (about ±1 %): it runs, with the CAN timing and the speed / tach readings on the HSI.

Now Y1 is a **YXC OT32258MJBA4SL** (LCSC C717687): 8 MHz CMOS oscillator, 3.2 × 2.5 mm, 1.8-3.3 V, ±20 ppm, -40 to
85 °C. Pinout from YXC's datasheet (YSO110TR series): 1 tri-state (high or open = on, tied to 3V3), 2 GND, 3 OUT
(to PC14), 4 VDD (100 nF beside it). Footprint `Oscillator_SMD_Abracon_ASE-4Pin_3.2x2.5mm`: the same pin order, pads
1.3 × 1.1 mm at ±1.05 / ±0.825 mm against YXC's recommended 1.3 × 1.2 at ±1.05 / ±0.95. The firmware selects
`RCC_HSE_BYPASS` and keeps the HSI fallback, so the same build runs on both board versions. GPSCard and RadioCard use
the same chip with the same crystal mistake.

## Pin map

| Pin | Port | Function | To |
| --- | --- | --- | --- |
| 1 | PB9 | GPIO in | PRESS_MON (oil feed present) |
| 2 | PC14 | OSC_IN (HSE bypass) | Y1 pin 3, 8 MHz oscillator |
| 3 | PC15 | unused | — |
| 6 | PF2 | NRST | J6 pin 4, 100 nF |
| 7 | PA0 | ADC1_IN0 | FUEL |
| 8 | PA1 | ADC1_IN1 | ENGINE_TEMP |
| 9, 10 | PA2, PA3 | USART2 TX, RX (AF1) | J8 sensor serial (TX through U7 to 5 V) |
| 11 | PA4 | ADC1_IN4 | OIL_PRESSURE |
| 12 | PA5 | ADC1_IN5 | AMB_TEMP |
| 13 | PA6 | ADC1_IN6 | ALT_L (39k / 10k) |
| 14 | PA7 | ADC1_IN7 | BATT_SENSE (100k / 22k) |
| 15 | PB0 | ADC1_IN8 | WAKE_SENSE (47k) |
| 16 | PB1 | ADC1_IN9 | IGN_SENSE (33k / 10k) |
| 17 | PB2 | GPIO in | TEMP_MON (temp feed present) |
| 18 | PA8 | TIM1_CH1 (AF2), input capture | TACH_SIG |
| 19, 21 | PA9, PA10 | USART1 TX, RX (AF1) | J7 debug serial |
| 20 | PC6 | GPIO in, pull-up | RPM_SEL_A (J5 1-2) |
| 22, 23 | PA11, PA12 | FDCAN1 RX, TX (AF3) | U3 |
| 24, 25 | PA13, PA14 | SWDIO, SWCLK | J6 |
| 26 | PA15 | GPIO out | PWR_HOLD (WAKE through D1) |
| 27 | PB3 | GPIO in, pull-up | RPM_SEL_B (J5 3-4) |
| 28 | PB4 | TIM3_CH1 (AF1), input capture | SPEED_SIG |
| 29 | PB5 | GPIO out | status LED |
| 30, 31 | PB6, PB7 | I2C1 SCL, SDA (AF6) | U4 FRAM, 4.7k pull-ups |
| 32 | PB8 | — | free |

## Firmware

[`firmware/sensor_card_stm32`](../firmware/sensor_card_stm32/README.md) (PlatformIO, the STM32duino core with ST's HAL
for the ADC scan and FDCAN1). It is rev A's firmware ([`firmware/sensor_card`](../firmware/sensor_card), which stays
for rev A cards) with only the hardware layer rewritten: the channel, calibration, odometer and framing code is built
from rev A's directory, one copy for both cards, so the frames, the CAN IDs and the FRAM layout are byte-compatible.
Counts stay rev A's 10-bit scale because the front end stayed ratiometric (the battery's are rescaled for the 3.3 V
reference in firmware). Demo mode is `demo on` on the console; the RPM-select jumpers are read on PC6 / PB3; the
console and the USB CAN link are USART1 on J7 (3.3 V). Builds clean and is host-tested; **not yet run on a card** (none
is built): that README's "Verify on hardware" lists what to check first, and "Flashing over SWD" how to load it
through J6.

## Status

**Schematic: done** (119 parts; 2026-10-26, clock changed 2026-11-13). **PCB: generated** by `gen_pcb.py` (placement + Freerouting on two layers):
see the git log for the routed state and the DRC result. Not yet checked: the QFN's decoupling by eye, the
front end against the DATASHEET's sender tables at 3.3 V, and the housing (nothing taller than rev A).

## Regenerating

As rev A ([`SensorCard/README.md`](../SensorCard/README.md#regenerating)), with `SensorCard2` in place of `SensorCard`
and no planes: Freerouting routes GND like any other net and the ground pours are filled afterwards. Re-apply the mask
dams (`python3 tools/mask_dams.py SensorCard2`) after a regeneration: the QFN is 0.5 mm pitch.
