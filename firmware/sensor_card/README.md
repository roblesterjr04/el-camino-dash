# Sensor card firmware (ATmega2560)

Firmware for the **sensor card** ([`SensorCard/`](../../SensorCard/README.md)): the dash's sensor pack on a DigiDash rev 2
slot card. An ATmega2560 at 16 MHz, 5 V, with an MCP2515 + MCP2562 on CAN as **fixed node `0x04`**, an MB85RC256V FRAM
for the odometer and the calibration, and the senders, speed, tach, ignition, battery and alternator inputs. It replaces
`arduino/sketches/sensor_pack` (ATmega328P) for the rev 2 board. The card's pins, the senders' jumpers and the
electrical numbers are in [`SensorCard/DATASHEET.md`](../../SensorCard/DATASHEET.md); how a reading is calibrated and
set from the menu is [`docs/protocols/sensor_calibration.md`](../../docs/protocols/sensor_calibration.md).

**Status:** builds clean for the 2560 (5.0 KB of 8 KB RAM, 66 KB of 248 KB flash); the logic is host-tested; **it has not
yet run on a card or a Mega**. The first bench run is the real test of the MCP2515 driver, the input capture and the
ADC scan: see [Bench bring-up](#bench-bring-up).

## What it does

| | |
| --- | --- |
| Every 50 ms | reads the senders (a background ADC scan averages ~70 samples per channel), the speed and tach pulse timing, ignition, alternator, battery; converts through the calibration; sends everything below |
| CAN `0x100`–`0x104`, 20 Hz | the frames the screens already read (`can_bus.md`). The ADC fields are *gauge counts*: the card sends the counts the screens' fixed formulas (`elc_sensor_units.h`) turn back into the **calibrated** value, and 0 for a channel that is not calibrated (the formulas' "out of range": 100 °F, 0 psi). The "wheel rpm" is in the screens' 2.1 m wheel, so the speed they show is the true speed whatever the sensor's pulses per mile |
| CAN SENSOR (`0x124`) | the center's core sensors 0–10 (speed km/h, rpm, fuel %, engine °C, oil kPa, charging, ambient °C, odometer m, distance to empty m, economy km/L now and trip) plus **11 battery volts**, with status (not calibrated, open, short, fuse fault) |
| CAN UI (`0x264`) | the card's menu page under Menu → Accessories → Sensor card: 64 items (readouts, bias / wiring / sender choices, captures, tables, speed and tach, service) in the center's units (gal / L, °F / °C, psi / kPa, from SYSTEM PREFS) |
| CAN ALERT (`0x024`) | one info alert while the key is on and the engine temperature or oil sender is not calibrated |
| CAN NET (`0x3C4`), heartbeat `0x704` | ANNOUNCE (type 3, product `0x0008`, unique ID from the FRAM), QUERY, IDENTIFY (blinks the LED), RESTART; heartbeat at 1 Hz with the "speaks channels" flag |
| J8 serial (USART1) | the 36-byte v4 sensor frame at 20 Hz (the fallback to the screens), the boot animation and FRAM lines as the old sensor pack printed them, and the center's `0xA5` keepalive in |
| J7 / USB (USART0) | the **bench console** and the **USB CAN link** ([`usb_link.md`](../../docs/protocols/usb_link.md)): on the bench `tools/can_relay.py` makes the computer the bus, no transceivers needed |
| FRAM | odometer (sensor_pack's v3 slots, so a stored mileage carries over), identity block, calibration in two CRC-checked copies written alternately |
| Key-off | on ignition falling, the calibration and the odometer's last fraction are written, and `PWR_HOLD` keeps the main board's rail for 3 s |
| J9 demo jumper | a slow fake sweep of everything (bench only, as before) |

Fuel level is damped over about 20 s (a refuel is followed after 10 s held with the engine off or the car stopped);
instant economy uses a lighter filter, as `sensor_pack` did. Open and short detection follows the calibration spec
(500 ms debounce; a 0–30 Ω fuel sender legitimately reads 0 counts when empty).

## Build and flash

PlatformIO (the platform is pinned in `platformio.ini`; the AVR toolchain installs itself):

```bash
cd firmware/sensor_card
pio run                                           # build
pio run -t upload --upload-port /dev/cu.usbmodemXXXX   # an Arduino Mega 2560 (stock bootloader, USB)
```

On the card itself:

- **With the Mega bootloader** (a bootloader on the 2560, then the J7 FTDI header takes uploads like a Mega): fuses
  `L 0xFF, H 0xD8, E 0xFD`, upload through J7 with `--upload-port` the FTDI adapter.
- **ISP only** (J6, with the card powered from its slot): `pio run -e sensorcard_isp -t fuses` then
  `pio run -e sensorcard_isp -t upload`. Fuses `L 0xFF, H 0xD9, E 0xFD`. Use a USBasp, AVRISP mkII or Atmel-ICE, not a USBtiny.

`pio test` is not used: the logic tests run on the Mac with the C++ compiler, no hardware:

```bash
firmware/sensor_card/test/host/run_tests.sh      # calibration, odometer + FRAM blocks, engine, channels, menu, frames
```

The shared CAN code is `firmware/elc_can`, built from its sources. Two small changes there for this card (defaults
unchanged for the ESP32 boards): `elc_chan.h` takes the channel buffer sizes from build flags (`ELC_CH_PAYLOAD_LIMIT`,
`ELC_CH_RX_BUFFERS`, `ELC_CH_RX_DUPS`; the card sets 160 / 3 / 16 because 8 KB of RAM cannot hold the 256 × 6 the ESP32
boards use), and `elc_usb_link.cpp` leaves the AVR's port code to `src/card_usb.cpp`. The card does not use
`ElcChannels` (its table of every node on the bus is 1.7 KB): `src/card_net.cpp` is the same protocol without it, and
never asks other nodes to describe themselves.

## On a Mega and a CAN shield (the bench)

The card's pins **are** the Arduino Mega's pins (that is how `card_pins.h` is written), so the same image runs on a
Mega 2560. What differs with a CAN-BUS shield:

| | Card | A Mega with a shield |
| --- | --- | --- |
| MCP2515 chip select | D53 (the SS pin) | D10 (SparkFun, Keyes, DFRobot, most clones) or D9 (Seeed v1.2). **The firmware tries D53, D10, D9 at boot** and uses the first MCP2515 that answers. Pin it with `can cs 9`, back to `can cs auto` |
| SPI | D50–D52 | The shield must use the **ICSP header** (Seeed v1.2 does; that header is the Mega's SPI). A shield wired for D11–D13 (the Uno's SPI) needs wires D11→D51, D12→D50, D13→D52 |
| MCP2515 crystal | 16 MHz | 16 MHz usually; some shields are 8 MHz: `can osc 8`, then `restart` |
| INT | D2 | Not needed: the driver polls |
| FRAM | on the card | None. The odometer reads "unknown" (fault bit 15 set, as designed) until you `odo set <miles>`, which then lives in RAM only. The calibration works but is not saved, so it is lost at reset. The unique ID is kept in the Mega's EEPROM |
| Inputs | pulled by the card | Floating on a bare Mega. Tie **D37 to 5 V for the demo sweep** (or to GND for none); to see real inputs tie D48 / D49 (speed, tach) high, D23 (ignition) as wanted, D25 / D26 high (feed monitors) |
| Termination | none on the card | The shield's 120 Ω jumper: leave it on only if the shield is at one end of the bus |

The Mega's USB is USART0, so `tools/can_relay.py` finds it as a serial port (opening it resets a Mega once: its DTR
auto-reset) and the Mega joins the bench bus with no CAN wiring at all; the real MCP2515 path and the USB link run side by
side. The USB link stays at 115200 baud (the ATmega's interrupt-driven serial cannot go faster), so the relay does not
switch this port to its fast rate.

## Bench console

115200 on USART0 (J7, or a Mega's USB); `help` lists it. US units (gal, °F, psi) unless suffixed `L`, `C`, `kPa`.

| Command | |
| --- | --- |
| `status` | node, ID, FRAM, calibration source, the live readings, faults |
| `can` · `can cs <pin\|auto>` · `can osc <8\|16>` | MCP2515 state and counters, the NET and USB link counters; the last two are saved in the EEPROM, then `restart` |
| `adc` | raw counts |
| `cal` · `cal <fuel\|temp\|oil> wiring pu\|pd` · `bias <ohms>` (`1k`, `100k`) · `preset <n>` · `offset <v>` · `capture <v>` · `point <raw>\|<ohms>R <v>` · `clear` | settings, points and live reading; `capture` is the menu's 5 s average |
| `cal fuel tank <gal>` · `cal amb ntc 100\|10` · `cal batt <volts>` · `cal speed ppm <n>` · `cal speed mile start` / `stop <miles>` · `cal tach ppr <n\|j5>` · `cal reset` · `cal save` | |
| `odo` · `odo set <miles>` | |
| `restart` | |

## Bench bring-up

Nothing here has run on silicon yet. In this order, each step isolates one part:

1. **Flash and `status`.** The LED is the center-heard indicator, so it stays dark until a center talks to the card. `status` should answer.
2. **`can`.** "MCP2515 up, CS 10" (or 9 or 53) means the SPI and the chip answered. If not: check the shield's SPI route (above) and try `can osc 8`.
3. **Demo sweep** (D37 high): `status` and the screens should sweep. With `tools/can_relay.py` running, the card appears as `sensor`; `sensor can` prints its counters.
4. **The center's menu.** Menu → Accessories should list *Sensor card* with the 64 items once the center is built to show accessory pages (`can_channels.md`, work to do).
5. **Real signals**: a function generator on D48 (speed) and D49 (tach), falling edges; `status` shows mph and rpm. 8000 pulses per mile: 22.2 Hz is 10 mph.

## Known gaps

- **The center does not yet back off `0x100`–`0x104` when it hears node `0x04`, nor relay `0x124` onto its core sensors** (`digidash_rev2_plan.md`, "Sensor pack on CAN"). Until it does, **do not wire J8 to the main board's J27 while the card is also on the CAN bus**: the center would re-send the same IDs with different data.
- The screens and console still read the legacy fields, so they show the calibrated values only through the gauge-count encoding above; reading `0x124` directly (and frame v5 on the serial link, which changes the wire format for the card and all three screens together) is not done.
- `cal export` / `cal import` (the whole calibration as a hex line) are not implemented; the center's SD backup covers a replacement card.
- LOG lines to the center's SD card are not sent.
- The GM coolant sensor preset's resistance table is entered from GM's published chart and is **unchecked**: capture a point against the real sender before trusting it, as the spec says.
- The stored bias resistance is a u32 (ohms ×10), not the spec's u16 (it cannot hold the 10 kΩ and 100 kΩ bank values); the FRAM block is private to the card, and the spec's payload table should read u32.
- `restart` (and the center's NET RESTART) jumps to address 0 instead of using the watchdog: the Mega's stock bootloader does not clear a watchdog reset and would loop.
