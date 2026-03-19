# El Camino Dash Wiring Pinout (HDMI-Only)

This project is now documented as HDMI-first with SPI display wiring dormant.

Reference baseline: Raspberry Pi 40-pin header mapping used on current Pi models (including Pi 4/Pi 5 header-compatible GPIO naming).

## Raspberry Pi connections

| Function | BCM GPIO | Physical Pin | Direction (Pi) | Notes |
|---|---:|---:|---|---|
| I2C SDA (MCP23017 + SI4703 + PT2314 control) | 2 | 3 | bidirectional | Shared I2C bus 1 |
| I2C SCL (MCP23017 + SI4703 + PT2314 control) | 3 | 5 | bidirectional | Shared I2C bus 1 |
| Arduino reset pulse (programming only) | 4 | 7 | output | Used by installer flash/reset helper |
| FM radio reset (SI4703 breakout reset) | 5 | 29 | output | `integrations.fm_radio.rst_pin` |
| IR receiver (gpio-ir overlay) | 26 | 37 | input | `dtoverlay=gpio-ir,gpio_pin=26` |
| Arduino UART TX -> Arduino RX | 8 (TXD4) | 24 | output | `uart4` / `/dev/ttyAMA3` |
| Arduino UART RX <- Arduino TX | 9 (RXD4) | 21 | input | `uart4` / `/dev/ttyAMA3` |
| ATtiny PI_SIGNAL (shutdown notice) | 16 | 36 | input | From ATtiny PB1 |
| ATtiny PI_CONFIRM (shutdown complete) | 12 | 32 | output | To ATtiny ADC3 |
| I2S BCLK -> PCM5102A BCK | 18 | 12 | output | PCM/I2S clock |
| I2S LRCLK -> PCM5102A LCK | 19 | 35 | output | PCM frame sync |
| I2S DIN -> PCM5102A DIN | 21 | 40 | output | Pi PCM_DOUT to DAC data in |
| 3.3V rail | - | 1 or 17 | power | Logic supply for MCP23017/ATtiny I/O side |
| 5V rail | - | 2 or 4 | power | Board power where required |
| Ground | - | any GND pin | ground | Common ground across Pi/Arduino/ATtiny/MCP23017 |

## MCP23017 (rotaries + menu buttons)

Assumed address and bus:

- I2C bus: `1`
- Address: `0x20` (decimal `32`)

Configured logical mapping (`inputs.mcp23017_controls`):

| Control | MCP23017 pin |
|---|---|
| Menu encoder A | GPA0 |
| Menu encoder B | GPA1 |
| Menu encoder push | GPA2 |
| Aux encoder A | GPA3 |
| Aux encoder B | GPA4 |
| Aux encoder push | GPA5 |
| Menu left button | GPA6 |
| Menu select button | GPA7 |

Notes:

- Internal pull-ups are enabled in software for the mapped MCP pins.
- Existing direct Pi GPIO rotary/button configs remain in files but are disabled.
- IR receiver should not be read through MCP23017; edge timing for IR decode is
  too tight for I2C expander polling/latency.

## IR receiver placement

The IR sensor can be mounted on the expansion board, but route its signal trace
to a direct Pi GPIO line (current config uses GPIO26 / pin 37 via
`dtoverlay=gpio-ir,gpio_pin=26`).

## FM module strategy

Current hardware strategy is the SparkFun FM Tuner Basic breakout (SI4703):

- Product: [SparkFun FM Tuner Basic Breakout - SI4703](https://www.sparkfun.com/sparkfun-fm-tuner-basic-breakout-si4703.html)
- Config selector: `integrations.fm_radio.chip: si4703`
- Note: weather-band controls in menu are SI4737-only legacy options.

## Audio source router (PT2314-D)

PT2314-D is used as a source-select/control stage between Pi DAC output and FM
module output.

- Config section: `integrations.audio_router`
- Active strategy: `chip: pt2314`, `protocol: i2c`, `address: 0x44` (decimal `68`)
- Source presets:
  - `pi_dac`: command byte `0xC0`
  - `fm_radio`: command byte `0xC1`

These command bytes are config-driven for board-to-board flexibility.

## Arduino sensor pack (vehicle senders)

All vehicle sender inputs are expected on the Arduino sensor pack, then forwarded to Pi over UART.

From `arduino/sketches/sensor_pack/sensor_pack.ino`:

| Signal | Arduino pin |
|---|---|
| Pot/Fuel sender | A0 |
| Thermistor/Temp sender | A1 |
| Pressure sender | A2 |
| Voltage sense | A3 |
| Hall speed pulse | D2 (interrupt) |
| Coil/RPM pulse | D3 (interrupt) |
| ALT indicator | D4 |
| RPM selector bit A | D5 (pull-up input, strap to GND for LOW) |
| IGN sense | D6 |
| Fault disable | D11 |
| RPM selector bit B | D12 (pull-up input, strap to GND for LOW) |

RPM pulses-per-revolution selector (`D5` = A, `D12` = B):

- `A=LOW`, `B=HIGH` -> `4` pulses/rev
- `A=HIGH`, `B=LOW` -> `6` pulses/rev
- `A=HIGH`, `B=HIGH` -> `8` pulses/rev
- `A=LOW`, `B=LOW` -> fallback to default (`8`)

## ATtiny power control unit

Provided ATtiny mapping (kept as source of truth):

| ATtiny pin/function | Direction vs ATtiny | Connected to |
|---|---|---|
| Pin 5 / PB0 / PI_POWER | output | 2N3904 base -> U3 ON/OFF |
| Pin 6 / PB1 / PI_SIGNAL | output | Pi GPIO16 (shutdown notice input) |
| Pin 7 / ADC1 / IGN_SENSE | input | Divider from IGN 12V |
| Pin 2 / ADC3 / PI_CONFIRM | input | Pi GPIO12 (shutdown complete output) |

### ATtiny ISP programming wiring (DIP-8)

Use this mapping when flashing `arduino/sketches/power_control/power_control.ino`:

| Programmer signal | ATtiny85 target pin |
|---|---|
| MOSI | Pin 5 (PB0/MOSI) |
| MISO | Pin 6 (PB1/MISO) |
| SCK | Pin 7 (PB2/SCK) |
| RESET | Pin 1 (RESET) |
| VCC | Pin 8 (VCC) |
| GND | Pin 4 (GND) |

Raspberry Pi default wiring used by script (`linuxspi` on SPI0):

| Raspberry Pi pin | ATtiny85 target pin |
|---|---|
| GPIO10 / pin 19 (SPI0_MOSI) | Pin 5 (PB0/MOSI) |
| GPIO9 / pin 21 (SPI0_MISO) | Pin 6 (PB1/MISO) |
| GPIO11 / pin 23 (SPI0_SCLK) | Pin 7 (PB2/SCK) |
| GPIO4 / pin 7 (RESET) | Pin 1 (RESET) |
| 3V3 / pin 1 | Pin 8 (VCC) |
| GND / pin 6 | Pin 4 (GND) |

Script:

```bash
bash arduino/scripts/program_power_control_attiny.sh --wiring
```

## Legacy SPI pin overlap

SPI display mode is intentionally dormant in current installer/runtime defaults.
With UART4 now used for Arduino comms, these pins are intentionally not available
for SPI wiring in the standard harness:

- GPIO7 (pin 26), GPIO8 (pin 24), GPIO9 (pin 21), GPIO10 (pin 19), GPIO11 (pin 23)

## Discrepancies corrected

- Removed active reliance on direct Pi GPIO rotary/button inputs in primary configs.
- Added MCP23017 control input path in code/config validation.
- Standardized FM reset pin usage to GPIO5 in non-SPI configs to avoid overlap with legacy GPIO17 rotary usage.
- Standardized Arduino serial path to `/dev/ttyAMA3` (UART4 on GPIO8/9) in Pi-targeted runtime configs.
