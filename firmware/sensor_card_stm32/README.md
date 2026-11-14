# Sensor card firmware, rev B (STM32G0B1)

Firmware for the **rev B sensor card** ([`SensorCard2/`](../../SensorCard2/README.md)): the dash's sensor pack on a
DigiDash slot card, now an **STM32G0B1KBU6** (Cortex-M0+, 64 MHz from an 8 MHz oscillator on PC14 in HSE bypass, 3.3 V) with the chip's own
**FDCAN1** through a TJA1051T/3 as **fixed CAN node `0x04`**, the MB85RC256V FRAM for the odometer and the
calibration, and the same senders, speed, tach, ignition, battery and alternator inputs as rev A. It is a port of
[`firmware/sensor_card`](../sensor_card/README.md) (rev A, ATmega2560), which stays as it is for rev A cards.

**Status:** builds clean for the G0B1 (98 KB of 128 KB flash, 9.5 KB of 144 KB RAM: the core's HAL and the C library are most of the flash); the logic is host-tested; **it has not run on a card: no rev B
board exists yet.** Everything in [Verify on hardware](#verify-on-hardware) is a guess until it does.

## What is shared and what is new

The card's logic did not change, and it is not copied: `platformio.ini` builds these **straight from rev A's
directory** (`build_src_filter`), with `../sensor_card/src` on the include path, so a fix there lands on both cards.

| From `../sensor_card/src` (one copy, both cards) | Does |
| --- | --- |
| `calib.*`, `odometer.*` | calibration, presets, captured points; the odometer and the FRAM blocks (byte-compatible: a rev A FRAM reads on a rev B card) |
| `engine.*`, `legacy.*` | the 50 ms step: conversions, faults, fuel damping, economy; frames `0x100`–`0x104` and the 36-byte serial frame |
| `card_net.*`, `card_chan.*`, `card_ui.*` | NET discovery, the SENSOR / ALERT / UI channels, the 64-item menu page |
| `../elc_can` | channel framing, the message codecs, the USB link records |

| Here (`src/`) | Replaces | Does |
| --- | --- | --- |
| `hw_sys.*` | the AVR fuses, `.init3`, `wdt.h`, EEPROM settings | the clock tree (`SystemClock_Config`: HSE 8 MHz → PLL → 64 MHz, **HSI fallback** if PC14 has no clock, shown by `status`), the reset cause, the chip's unique ID, the 2 s independent watchdog, restart |
| `hw_adc.*`, `adc_scale.h` | the ADC interrupt scan | ADC1 + DMA, the 8 inputs in a ring of the last 32 scans, 16× hardware oversampling, no interrupt; counts rescaled to **rev A's 10-bit scale** (below); ignition and alternator L through their dividers with hysteresis |
| `hw_capture.*`, `pulse_track.h` | Timer 4 / 5 input capture | TIM1_CH1 (tach, PA8) and TIM3_CH1 (speed, PB4) input capture on the falling edge, 0.5 µs ticks, the 16-bit counters extended by the overflow interrupt, the same hold-offs (`pulse_track.h` is the AVR logic, host-tested) |
| `hw_fdcan.*` | `mcp2515.*` | FDCAN1 in classic mode, single-shot, 24-frame software queue in front of the 3-deep hardware FIFO, receive by interrupt into a 32-frame ring, bus-off recovery after 1 s |
| `fram.*` | `fram.cpp` | the FRAM on I2C1 (PB6 / PB7), 400 kHz, 24-byte chunks |
| `card_usb.cpp` | `card_usb.cpp` | this card's end of the USB link on USART1 (`ELC_USB_PORT_EXTERNAL` keeps `elc_usb_link.cpp`'s ESP32 port out of the build) |
| `main.cpp`, `console.cpp`, `card_state.h`, `card_pins.h` | the same files | rev A's, with the AVR parts swapped for the above, `demo on|off` on the console in place of the J9 jumper, and no `can cs` / `can osc` (nothing to set) |

**Counts.** The screens' formulas, the calibration tables and the stored points are all in rev A's 10-bit counts of a
5 V reference. On rev B the senders' pull-up banks run from the same 3.3 V the ADC measures against, so a sender gives
the **same fraction of full scale**; `adc_scale.h` maps 0–4095 onto 0–1023 (×1023/4095, the exact ratiometric
equivalent of the README's "12-bit >> 2") and nothing downstream knows the difference. The **battery** divider is the
same 100k / 22k but the reference changed, so its counts are rescaled by 3.3 / 5 as well: `cal::battery_volts` and the
`batt_trim_ppm` in the FRAM are unchanged (full scale is now 18.3 V). **Ignition** (33k / 10k) and **alternator L**
(39k / 10k) were digital inputs on rev A and sit on ADC pins on rev B; they are read through the ADC and switch at the
slot voltages rev A's thresholds worked out to (ignition on above 7.5 V, off below 3.75 V; L high above 9.6 V, low below
4.8 V), with rev A's two-step debounce kept in `main.cpp`.

## Why the STM32duino core and not CubeMX / HAL alone

Every other board in this repo is Arduino-framework PlatformIO, the shared code is written against `Serial`-shaped
ports, `millis()` and `snprintf`, and the GPS card's README already names STM32duino for the same chip. So this uses
the **STM32duino core** (`framework = arduino` on `ststm32`; `Serial`, a second `Uart` for J8, `Wire` for the FRAM,
`HardwareTimer` for the two captures, `IWatchdog`, `millis`) and calls **ST's HAL**, which the core ships (ADC and DMA
on by default; FDCAN switched on with `HAL_FDCAN_MODULE_ENABLED` in `platformio.ini`), **directly** for the two
things the core has no API for: the DMA ADC scan and FDCAN1.
No third-party CAN library: the FDCAN driver is 200 lines in `hw_fdcan.cpp` over the HAL, and the core even routes
FDCAN1's interrupt lines (which the G0 shares with TIM16 / TIM17) to a handle the sketch names (`phfdcan1`), so receive
runs on interrupt without touching a vector. A pure HAL / LL project would have meant writing the UART rings, the I2C
master, the tick and the start-up code the core already provides, for no gain on a card that never sleeps and has 144 KB
of RAM. The two things the core does by itself that this firmware overrides: its clock setup (HSI only on a generic
variant: `hw_sys.cpp` supplies `SystemClock_Config` for the HSE oscillator) and `Serial`'s pins (`SERIAL_UART_INSTANCE=1`,
`PIN_SERIAL_RX/TX`, in `platformio.ini`).

The board is a custom definition, [`../boards/sensorcard2_g0b1kb.json`](../boards/sensorcard2_g0b1kb.json)
(PlatformIO ships no generic G0B1KB board): the core's generic `G0B1K(B-C-E)(T-U)` variant, `ARDUINO_GENERIC_G0B1KBUX`,
128 KB flash, 144 KB RAM, ST-Link upload. The platform is pinned (`ststm32@20.0.0`, core 2.x as `~4.30000.0`).

## Build

PlatformIO (the platform and the ARM toolchain install themselves on the first run):

```bash
cd firmware/sensor_card_stm32
pio run
```

`pio test` is not used: the logic tests run on the Mac with the C++ compiler, no hardware. This runs rev A's suite on
the shared code and then this card's own (count scaling, level thresholds, capture timestamps):

```bash
firmware/sensor_card_stm32/test/host/run_tests.sh
```

## Flashing over SWD

The STM32 arrives blank and has no bootloader in the loop: everything goes in through **J6** (1×5 SWD: see the card's
README for the pinout: SWDIO = PA13, SWCLK = PA14, NRST, 3.3 V sense, GND), with the card **powered from its slot** (or
3.3 V on the SWD header's VDD pin, if the probe supplies it and the card is out of the slot). An ST-Link V2 (clones
work), or any CMSIS-DAP / J-Link probe. There is no BOOT0 strap: on the G0 the factory option bytes boot from flash.

With PlatformIO (OpenOCD, installed on first use):

```bash
pio run -t upload
```

Other probes: `upload_protocol = cmsis-dap` or `jlink` on the command line (`pio run -t upload --upload-protocol cmsis-dap`).

With ST's tools instead, the same `.pio/build/sensorcard2/firmware.bin`:

```bash
STM32_Programmer_CLI -c port=SWD -w .pio/build/sensorcard2/firmware.bin 0x08000000 -v -rst
```

or `st-flash write .pio/build/sensorcard2/firmware.bin 0x8000000` (the open-source stlink tools).

A chip that was locked (RDP level 1) by an earlier experiment needs `STM32_Programmer_CLI -c port=SWD -ob RDP=0xAA`
first, which also erases it. The option byte `nBOOT_SEL` stays at its default (boot from flash, BOOT0 pin ignored) and
nothing here changes it.

Debug: `pio debug` with the same probe (`debug_tool = stlink` is the board's default).

## On the bench

There are no bench builds: this is the image that ships, and the bench reaches the bus through the **USB CAN link**
(`tools/can_relay.py`, [`usb_link.md`](../../docs/protocols/usb_link.md)) on **J7**, which on rev B is **3.3 V logic**
(a 3.3 V FTDI / CP2102 cable: a 5 V one damages PA10). The relay finds the card as a serial port, the card joins the bench
bus with no transceiver wiring, and the real FDCAN path and the USB link run side by side. The link stays at 115200
baud (the card's status record says so and the relay does not switch it: faster rates have not been tried).

Bench setup differs from rev A:

| | Rev A | Rev B |
| --- | --- | --- |
| Demo sweep | the J9 jumper | `demo on` on the console (off again at a restart) |
| CAN shield settings | `can cs`, `can osc` | gone: the controller is built in |
| Inputs on a bare board | floating | the same: the senders' banks, the 22k on the tach opto and the 10k on the speed input are the pull-ups; the feed monitors and ignition read low until fed |
| Unique ID | EEPROM, or noise | the chip's 96-bit ID, hashed (the FRAM identity block still wins once written) |

## Bench console

115200 8N1 on USART1 (J7); `help` lists it. US units (gal, °F, psi) unless suffixed `L`, `C`, `kPa`.

| Command | |
| --- | --- |
| `status` | node, ID, clock source (HSE, or HSI if the oscillator gave no clock), reset cause, FRAM, calibration source, the live readings, faults |
| `can` | FDCAN1 state (error state, the error counters, the last error code), tx sent / acked / dropped, rx / lost / overflowed, bus-off count; the NET and USB link counters |
| `adc` | the counts the calibration sees (rev A's scale), the raw 12-bit samples of all eight inputs, and the ignition, alternator and battery lines in slot volts |
| `demo on|off` | the slow fake sweep of everything (bench only) |
| `cal …`, `odo …`, `restart` | as rev A (`cal` · `cal <fuel\|temp\|oil> wiring pu\|pd` · `bias <ohms>` · `preset <n>` · `offset <v>` · `capture <v>` · `point <raw>\|<ohms>R <v>` · `clear` · `cal fuel tank <gal>` · `cal amb ntc 100\|10` · `cal batt <volts>` · `cal speed ppm <n>` · `cal speed mile start` / `stop <miles>` · `cal tach ppr <n\|j5>` · `cal reset` · `cal save` · `odo` · `odo set <miles>`) |

Reset cause bits (`status`): 01 power-on / brown-out, 02 NRST pin, 04 option-byte loader, 08 independent watchdog,
10 window watchdog, 20 software (`restart`, NET RESTART), 40 low-power.

## Verify on hardware

Nothing below has run on silicon. In this order, each step isolates one part:

1. **Flash and `status`.** The console must come up and say `clock HSE 8 MHz`. `HSI (no HSE clock)` means no clock on PC14 (Y1 the
   8 MHz oscillator, its supply, or a first rev B board, which has a crystal that cannot start): everything still runs, but the CAN bit timing is then on the HSI's ±1 % and the UARTs on its
   drift, so fix it before trusting anything else.
2. **`can`** should read `FDCAN1 up at 500 kbit/s, error active`. With `tools/can_relay.py` running the card appears
   as `sensor`. On a real bus with another node, `tx sent` and `acked` should climb together; `acked` stuck at 0 with
   `error passive` means nobody acknowledges (the transceiver, the pins, or the bit timing: PA11 RX / PA12 TX, AF3, no
   PA9/PA10 remap bits set in SYSCFG). The sample point is 75 % of 16 quanta at 8 MHz, the ESP32 boards' timing.
3. **`adc` with nothing connected**, then with a known resistor on each sender input: the raw 12-bit value should be
   4095 × R / (R + bank) and the rev A count that / 4. Battery: `adc` prints the slot voltage from the 100k / 22k
   divider; compare with a meter and set `cal batt <volts>` if the divider needs a trim.
4. **Ignition and alternator L**: feed the slot's switched 12 V from a bench supply and sweep it; `adc` shows the
   volts and the state, which must switch on at 7.5 V and off at 3.75 V (L: 9.6 / 4.8 V). The thresholds are
   `card_pins.h` constants.
5. **`demo on`**: `status` and the screens should sweep.
6. **Real signals**: a function generator on J3 (speed) and J4 (tach), falling edges; `status` shows mph and rpm.
   8000 pulses per mile: 22.2 Hz is 10 mph. Check the tach's input filter (8 samples at 64 MHz) is enough against the
   opto's edges; it is a `FILTER_CKINT_N8` in `hw_capture.cpp` if a longer one is needed.
7. **FRAM**: `odo set`, `cal save`, power-cycle, `status` says `calibration from FRAM` and the odometer is back. The
   I2C driver's timeout is 100 ms per transfer, so a missing FRAM costs a slow boot (20 probes), not a hang.
8. **Key-off**: drop the switched 12 V with the engine "running" (demo); WAKE (PA15) must stay high about 3 s and the
   FRAM get written.
9. **The watchdog**: `status` after a deliberate hang (none is built in) reports reset cause 08.

Things the port assumes that the schematic should be checked against before the first flash:

- **PA11 / PA12 are the FDCAN pins and PA9 / PA10 the USART1 pins** with SYSCFG's PA11 / PA12 remap off (the reset
  default; nothing here sets it). On this package they are four separate pins.
- **J7 is 3.3 V** (rev A's was 5 V): the FTDI cable must be a 3.3 V one.
- **PA15 (PWR_HOLD) and PB3 (RPM_SEL_B)** are plain GPIO on the G0 at reset (no JTAG), so no debug-port release is needed.
- The ADC's 10k series resistors with 160.5 cycles of sampling at 16 MHz (10 µs) are well inside the G0's source
  impedance limit; if the raw readings droop with the oversampling on, drop the ADC clock to PCLK / 8 in `hw_adc.cpp`.
- The input-capture pins have no internal pull-ups enabled (the card's own are the pull-ups), as on rev A.

## Known gaps

Rev A's list applies (the center not backing off `0x100`–`0x104`, frame v5, `cal export` / `import`, LOG lines, the
unchecked GM coolant table, the u32 bias). Rev B's own:

- **Nothing has run on a card.** The HAL calls were written against the core's headers and build clean; the
  configurations (ADC fixed-sequencer + oversampling + circular DMA, FDCAN bit timing and FIFO modes, the capture
  filter) are what the reference manual says, not what a scope has seen.
- The USB link stays at 115200 baud (the ESP32 boards switch up to 2 Mbit/s). The core's UART is interrupt-per-byte;
  460800 would likely work on the M0+ at 64 MHz and is worth trying once the card exists.
- The HSI fallback keeps the console alive if the oscillator fails but **does not retune the FDCAN** for it (the prescaler
  is for 64 MHz either way; the HSI gets to 64 MHz too, but at ±1 %, which CAN tolerates and a UART tolerates).
- `demo` does not survive a restart (rev A's jumper did). If that matters on the bench, a byte in the FRAM identity
  block is the place to keep it.
