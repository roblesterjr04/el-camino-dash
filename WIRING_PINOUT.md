# El Camino Dash Wiring Pinout (HDMI-Only)

This project is now documented as HDMI-first with SPI display wiring dormant.

Reference baseline: Raspberry Pi 40-pin header mapping used on current Pi models (including Pi 4/Pi 5 header-compatible GPIO naming).

## Raspberry Pi connections

| Function | BCM GPIO | Physical Pin | Direction (Pi) | Notes |
|---|---:|---:|---|---|
| I2C SDA (SI4703 + PT2314 control) | 2 | 3 | bidirectional | Shared I2C bus 1 |
| I2C SCL (SI4703 + PT2314 control) | 3 | 5 | bidirectional | Shared I2C bus 1 |
| Arduino reset pulse (programming only) | 4 | 7 | output | Used by installer flash/reset helper |
| FM radio reset (SI4703 breakout reset) | 5 | 29 | output | `integrations.fm_radio.rst_pin` |
| IR via **gpio-ir** (optional; disable if using encoder IR) | 26 | 37 | input | `dtoverlay=gpio-ir,gpio_pin=26` |
| **Encoder / vehicle MCU UART TX → MCU RX** | **14 (TXD0)** | **8** | output | **Primary UART only** — `/dev/serial0` |
| **Encoder / vehicle MCU UART RX ← MCU TX** | **15 (RXD0)** | **10** | input | **Primary UART only** — `/dev/serial0` |
| ATtiny PI_SIGNAL (shutdown notice) | 16 | 36 | input | From ATtiny PB1 |
| ATtiny PI_CONFIRM (shutdown complete) | 12 | 32 | output | To ATtiny ADC3 |
| I2S BCLK -> PCM5102A BCK | 18 | 12 | output | PCM/I2S clock |
| I2S LRCLK -> PCM5102A LCK | 19 | 35 | output | PCM frame sync |
| I2S DIN -> PCM5102A DIN | 21 | 40 | output | Pi PCM_DOUT to DAC data in |
| 3.3V rail | - | 1 or 17 | power | Logic reference as needed for level-safe interfaces |
| 5V rail | - | 2 or 4 | power | Board power where required |
| Ground | - | any GND pin | ground | Common ground across Pi/Arduinos/ATtiny |

There is **no second Pi UART** in this design: encoders, OLED downlink, and any future Nano-side traffic share **TXD0/RXD0**. Vehicle analog/RPM data use **`inputs.sensor_pack_i2c`** on I2C, not a separate UART.

## Encoder Arduino (ATmega328P)

Rotary/menu controls and SPI OLED are handled on the encoder MCU. The Pi opens **`/dev/serial0`** at **`115200`** for `inputs.encoder_arduino_serial` and `integrations.nano_bridge` (same FD when encoder serial is enabled).

ATmega328P control-side pin map:

| Control | ATmega328P pin |
|---|---|
| Menu encoder A | D2 |
| Menu encoder B | D3 |
| Menu encoder push | D4 |
| Aux encoder A | D5 |
| Aux encoder B | D6 |
| Aux encoder push | D7 |
| Menu left button | D8 |
| Menu select button | D9 |
| **IR demodulator OUT** (38 kHz module, e.g. VS1838B) | **A0** (digital in; 3V3 **VCC**/GND) |

NEC IR is decoded on the encoder MCU (**Arduino-IRremote** v4). Map remote keys in `applyNecCommand()` inside `encoder_controller.ino`; the Pi receives `IR=<token>` on the same UART line as encoders. Leave **`inputs.ir_remote.enabled: false`** when using this path.

SPI OLED (**SSD1322 256×64**, U8g2 `SSD1322_NHD_256X64`) on the **encoder** ATmega328P uses hardware SPI and leaves D2–D9 free for encoders/buttons:

| OLED signal | ATmega328P pin |
|---|---|
| MOSI | D11 |
| SCK | D13 |
| CS | D10 |
| **DC** (data/command) | **A4** — must match `OLED_DC` in `encoder_controller.ino` |
| **RST** | **A5** — must not be swapped with **DC**; swapping DC/RST produces full-screen snow/garbage |
| VCC / GND | 3V3 or 5V per module (logic must match level shifters if any) |

Some 256×64 modules use a **ZJY** SSD1322 variant; if the NHD profile does not initialize, switch the sketch to `U8G2_SSD1322_ZJY_256X64_1_4W_HW_SPI` (same wiring).

**Speckles / snow:** Usually SPI too fast for the wiring (default **`OLED_SPI_MAX_HZ`** is **400 kHz** in the sketch; lower further if needed). First verify **DC** and **RST** are not crossed. Also **GND** common, short MOSI/SCK/CS/DC leads, and 3V3 logic levels. Wrong controller profile (NHD vs ZJY) or a **128×64 SSD1306** panel mis-labeled as 256×64 will also look like garbage—match the driver to the chip.

Serial framing emitted by `arduino/sketches/encoder_controller/encoder_controller.ino`:

- One line per event batch, newline-terminated.
- Format: `MD=<int>,AD=<int>,MP=<count>,AP=<count>` optional `,IR=<token>` for Pi-side actions.
- `MD`/`AD` are signed deltas since previous transmission.
- `MP`/`AP` are cumulative press counters.

## IR receiver placement

**Preferred (plan):** mount the demodulator on the **encoder** board, **OUT → ATmega328P A0** (see table above).

**Alternate:** route to the Pi (GPIO26 / pin 37, `dtoverlay=gpio-ir,gpio_pin=26`) and enable **`inputs.ir_remote`** — do **not** use both paths at once for the same remote actions.

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

Production path: **I2C** sensor frames (`inputs.sensor_pack_i2c`) from the ATmega328 sensor pack — **no UART** to the Pi for gauges.

**Temperature semantics on the Pi:** `analog_inputs.coolant_temp` is **engine coolant** only. `analog_inputs.ambient_temp` is **outside / ambient air** (OLED `WT`, clock “OUTSIDE …” label). **I2C** `sensor_pack_i2c` currently publishes coolant in the frame’s temp field only — ambient is **not** in that 16-byte map yet, so those installs will show `--` for outside until the pack firmware/protocol adds it or you use another source. **UART** `inputs.arduino_serial` **extended** packets (21-byte payload after sync) include ambient ADC bytes `[8:9]` mapped to `analog_inputs.ambient_temp` (see `sensor_pack.ino` / A4).

The legacy `arduino/sketches/sensor_pack/sensor_pack.ino` UART binary protocol remains for bench bring-up only; it cannot run concurrently with the encoder on the same Pi UART.

Pin map (sensor MCU), from `sensor_pack.ino`:

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
SPI0 may still be used for ATtiny programming scripts. GPIO7/8/9/10/11 are not assigned to a **second UART** in this design.

## Discrepancies corrected

- Removed active reliance on direct Pi GPIO rotary/button inputs in primary configs.
- Replaced MCP23017 controls with dedicated encoder Arduino serial input.
- Standardized FM reset pin usage to GPIO5 in non-SPI configs to avoid overlap with legacy GPIO17 rotary usage.
- **Single Pi UART:** encoder + OLED downlink use **`/dev/serial0`** (TXD0/RXD0) only; image boot config does **not** enable `uart4`.
