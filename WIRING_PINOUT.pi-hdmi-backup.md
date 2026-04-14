# El Camino Dash Wiring Pinout (HDMI-Only)

The vehicle UI is **HDMI-first** on the Raspberry Pi. The **encoder + SPI OLED** stack can be either a **Raspberry Pi Pico ©2020** (RP2040; the original Pico board — current `encoder_controller.ino` default for Pico builds) or a legacy **ATmega328P** board. Physical pin numbers below match that **Pico** land pattern (not Pico W / Pico 2 unless you re-map). Pi header mapping follows the **40-pin** layout used on Pi 4 / Pi 5 (BCM GPIO numbers).

## Raspberry Pi connections


| Function                                                   | BCM GPIO       | Physical Pin | Direction (Pi) | Notes                                                            |
| ---------------------------------------------------------- | -------------- | ------------ | -------------- | ---------------------------------------------------------------- |
| I2C SDA (SI4703 + PT2314 control)                          | 2              | 3            | bidirectional  | Shared I2C bus 1                                                 |
| I2C SCL (SI4703 + PT2314 control)                          | 3              | 5            | bidirectional  | Shared I2C bus 1                                                 |
| Arduino reset pulse (programming only)                     | 4              | 7            | output         | Used by installer flash/reset helper                             |
| FM radio reset (SI4703 breakout reset)                     | 5              | 29           | output         | `integrations.fm_radio.rst_pin`                                  |
| IR via **gpio-ir** (optional; disable if using encoder IR) | 26             | 37           | input          | `dtoverlay=gpio-ir,gpio_pin=26`                                  |
| **Sensor pack ATmega328P UART** (Pi **UART1**)             | *dtoverlay*    | *varies*     | bidirectional  | `inputs.arduino_serial` — e.g. `/dev/ttyAMA1` (verify on device) |
| **Encoder MCU UART** (Pi **UART4**)                        | 8 (TX), 9 (RX) | 10, 11       | bidirectional  | `encoder_arduino_serial` + `nano_bridge` — e.g. `/dev/ttyAMA4`   |
| ATtiny PI_SIGNAL (shutdown notice)                         | 16             | 36           | input          | From ATtiny PB1                                                  |
| ATtiny PI_CONFIRM (shutdown complete)                      | 12             | 32           | output         | To ATtiny ADC3                                                   |
| I2S BCLK -> PCM5102A BCK                                   | 18             | 12           | output         | PCM/I2S clock                                                    |
| I2S LRCLK -> PCM5102A LCK                                  | 19             | 35           | output         | PCM frame sync                                                   |
| I2S DIN -> PCM5102A DIN                                    | 21             | 40           | output         | Pi PCM_DOUT to DAC data in                                       |
| 3.3V rail                                                  | -              | 1 or 17      | power          | Logic reference as needed for level-safe interfaces              |
| 5V rail                                                    | -              | 2 or 4       | power          | Board power where required                                       |
| Ground                                                     | -              | any GND pin  | ground         | Common ground across Pi / MCUs / ATtiny                          |


Vehicle UART split (see `config/dashboard.yaml` / `dashboard.vehicle.yaml`):

- **UART4** — Pico or encoder MCU uplink/downlink (`inputs.encoder_arduino_serial`, `integrations.nano_bridge`), **115200**, Pi **GPIO 8 = TX**, **GPIO 9 = RX** (enable with `dtoverlay=uart4` in `/boot/firmware/config.txt`).
- **UART1** — ATmega328P sensor pack (`inputs.arduino_serial`), **115200**, binary frames from `sensor_pack.ino`.

**Pi ↔ Pico UART wiring (3.3 V logic, common GND):**


| Signal               | Raspberry Pi (UART4)               | Raspberry Pi Pico ©2020       |
| -------------------- | ---------------------------------- | ----------------------------- |
| Pi transmits to Pico | GPIO **9** (UART4 RX — pin **11**) | **GP1** (UART RX — pin **2**) |
| Pico transmits to Pi | GPIO **8** (UART4 TX — pin **10**) | **GP0** (UART TX — pin **1**) |
| Ground               | Any GND                            | Any GND                       |


Cross TX/RX as usual: **Pico GP0 (TX) → Pi RX (GPIO 9)**; **Pi TX (GPIO 8) → Pico GP1 (RX)**.

**No Pi I2C to the sensor pack:** the ATmega328P sensor MCU runs at **5V**; we do **not** use an I2C link to the Pi for gauges (3.3V I2C would require level translation). **UART** is the only Pi↔sensor-pack data path in shipping configs. The Pi **I2C bus** is still used for other peripherals (e.g. SI4703 FM, PT2314 router), not for the sensor ATmega328P.

Exact `**/dev/tty…`** names depend on Pi model and `config.txt` / `dtoverlay`; use `ls /dev/ttyAMA*` on the vehicle Pi to confirm (UART4 is typically `**/dev/ttyAMA4**`).

---

## Encoder: Raspberry Pi Pico ©2020 (RP2040)

Board: **Raspberry Pi Pico** (RP2040, ©2020 Raspberry Pi Ltd) — the **original** DIP-40 Pico module. **Physical pin** numbers in the table below are the **pin numbers printed on that board** (USB connector at the bottom edge; pin 1 is GP0 next to the connector).

`arduino/sketches/encoder_controller/encoder_controller.ino` maps the **Pi link** to `**Serial1`** on **GP0 / GP1** (hardware **UART0**), **115200**. **USB** is optional: native USB CDC is used only for **debug** (`ELC_USB_DEBUG` in the sketch; boot summary + `[usb] heartbeat` every 2 s when a host opens the port). The vehicle does **not** require USB connected for Pi UART or SPI OLED.

### Pico GPIO summary (`encoder_controller.ino`)


| Function                                  | GPIO | Pico ©2020 physical pin |
| ----------------------------------------- | ---- | ----------------------- |
| **UART TX → Pi**                          | GP0  | 1                       |
| **UART RX ← Pi**                          | GP1  | 2                       |
| Menu encoder A                            | GP2  | 4                       |
| Menu encoder B                            | GP3  | 5                       |
| Menu encoder push                         | GP4  | 6                       |
| Aux encoder A                             | GP5  | 7                       |
| Aux encoder B                             | GP6  | 8                       |
| Aux encoder push                          | GP7  | 9                       |
| Menu left button                          | GP8  | 10                      |
| Menu select button                        | GP9  | 11                      |
| SPI0 **SCK**                              | GP18 | 24                      |
| SPI0 **MOSI**                             | GP19 | 25                      |
| SPI0 **MISO**                             | GP16 | 21                      |
| OLED **CS**                               | GP17 | 22                      |
| OLED **DC**                               | GP20 | 26                      |
| OLED **RST**                              | GP21 | 27                      |
| IR demodulator OUT (38 kHz, e.g. VS1838B) | GP22 | 29                      |


NEC IR is decoded on the Pico (**Arduino-IRremote** v4). Map remote keys in `applyNecCommand()`; the Pi receives `IR=<token>` on the same UART line as encoders. Leave `**inputs.ir_remote.enabled: false`** when using this path.

### SPI OLED (SSD1322 256×64) on Pico ©2020

Hardware **SPI0** with explicit pins in the sketch (`SPI.setSCK` / `setTX` / `setRX` on arduino-pico):


| OLED signal | Pico GPIO | Notes                                        |
| ----------- | --------- | -------------------------------------------- |
| MOSI        | GP19      | SPI0 TX                                      |
| SCK         | GP18      | SPI0 SCK                                     |
| MISO        | GP16      | SPI0 RX (used by HW SPI)                     |
| CS          | GP17      |                                              |
| **DC**      | GP20      | Must match module D/C — do not swap with RST |
| **RST**     | GP21      | Swapping DC/RST produces snow/garbage        |


Some 256×64 modules use a **ZJY** SSD1322 variant; if the NHD profile does not initialize, define `**ELC_OLED_SSD1322_ZJY`** in the sketch. For a true **128×64 SSD1306**, define `**ELC_OLED_SSD1306_128X64`** (different wiring expectations).

**Speckles / snow:** Lower `**OLED_SPI_MAX_HZ`** (default **400 kHz** in the sketch). Verify **DC** vs **RST**, **GND** common, and short leads.

### Pico green LED: four fast + four slow blinks

If the onboard **green LED** (GP25) repeats **four short (rapid) flashes** then **four long flashes**, you are almost certainly running a build made with **Arduino Mbed OS for RP2040** (`arduino:mbed_rp2040:pico`). That pattern is a **documented Mbed RTOS fault / crash loop**, not a signal from `encoder_controller.ino` (the sketch does not drive the LED). **Fix:** install the **Earle Philhower** core (**arduino-pico**), select **Raspberry Pi Pico** under that package, and re-flash (project scripts default to `**rp2040:rp2040:rpipico`**). See `arduino/README.md` (Encoder Controller on Pi Pico).

### Serial protocol (unchanged; Pico or ATmega)

Frames from `encoder_controller.ino`:

- One uplink line per event batch, newline-terminated.
- Format: `MD`/`AD` as `UP`/`DOWN` per detent (or `0` when idle; legacy signed ints still accepted), with `MP=<count>,AP=<count>` optional `,IR=<token>`.
- Pi → OLED downlink lines must include `**MODE=**`, `**TITLE=**`, and `**|**` or they are ignored (see sketch `pollDownlink()`).

---

## Encoder: ATmega328P (legacy)

Rotary/menu controls and SPI OLED can still be built on an **ATmega328P**. The Pi opens the **same encoder UART device** (vehicle default **UART4**, e.g. `/dev/ttyAMA4`) at `**115200`** for `inputs.encoder_arduino_serial` and `integrations.nano_bridge`.


| Control                                              | ATmega328P pin                       |
| ---------------------------------------------------- | ------------------------------------ |
| Menu encoder A                                       | D2                                   |
| Menu encoder B                                       | D3                                   |
| Menu encoder push                                    | D4                                   |
| Aux encoder A                                        | D5                                   |
| Aux encoder B                                        | D6                                   |
| Aux encoder push                                     | D7                                   |
| Menu left button                                     | D8                                   |
| Menu select button                                   | D9                                   |
| **IR demodulator OUT** (38 kHz module, e.g. VS1838B) | **A0** (digital in; 3V3 **VCC**/GND) |


SPI OLED (**SSD1322 256×64**, U8g2 `SSD1322_NHD_256X64`) on the **encoder** ATmega328P uses hardware SPI:


| OLED signal | ATmega328P pin                                            |
| ----------- | --------------------------------------------------------- |
| MOSI        | D11                                                       |
| SCK         | D13                                                       |
| CS          | D10                                                       |
| **DC**      | **A4** — must match `OLED_DC` in `encoder_controller.ino` |
| **RST**     | **A5** — must not be swapped with **DC**                  |


Some 256×64 modules use a **ZJY** SSD1322 variant; if the NHD profile does not initialize, switch the sketch to `U8G2_SSD1322_ZJY_256X64_1_4W_HW_SPI` (same wiring).

## IR receiver placement

**Preferred:** mount the demodulator on the **encoder** board — **OUT → Pico GP22** or **ATmega A0** (see tables above).

**Alternate:** route to the Pi (GPIO26 / pin 37, `dtoverlay=gpio-ir,gpio_pin=26`) and enable `**inputs.ir_remote`** — do **not** use both paths at once for the same remote actions.

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

**Production path:** `**inputs.arduino_serial`** — UART binary frames from `sensor_pack.ino` on **Pi UART1** (e.g. `/dev/ttyAMA1`), **115200**, separate from **UART4** (encoder/Pico). **Do not** use `**inputs.sensor_pack_i2c`** for the 5V ATmega328P sensor pack on the Pi without level shifters; vehicle images keep **I2C sensor input disabled** and use **UART only**.

`**inputs.sensor_pack_i2c`** remains in the codebase for legacy bench / alternate 3.3V I2C slave builds only.

**Temperature semantics on the Pi:** `analog_inputs.coolant_temp` is **engine coolant** only. `analog_inputs.ambient_temp` is **outside / ambient air** (OLED `WT`, clock “OUTSIDE …” label). **UART** `inputs.arduino_serial` **extended** packets (21-byte payload after sync) include ambient ADC bytes `[8:9]` mapped to `analog_inputs.ambient_temp` (see `sensor_pack.ino` / A4).

lets The `arduino/sketches/sensor_pack/sensor_pack.ino` UART binary protocol must use a **different** Pi serial device than the encoder link (vehicle: **UART1** vs **UART4**).

### Temperature (A1 coolant, A4 ambient) — 5 V and 100 kΩ divider

The sensor MCU should run at **5 V** (`AVCC` / default `analogReference()`). `**sensor_pack.ino`** assumes `**AREF_VOLTAGE 5.0**`, `**THERM_SERIES_R 100000**`, `**THERM_NOMINAL_R 100000**` (100 kΩ NTC @ 25 °C), and `**THERM_BETA 3950**` (typical 100 kΩ glass NTC curve).

The Steinhart–Hart path uses the usual divider relation after an optional ADC invert. **Two wirings match the math:**


| Firmware `ENGINE_TEMP_INVERT_ADC` / `AMBIENT_TEMP_INVERT_ADC` | Physical divider (each thermistor channel)                                         |
| ------------------------------------------------------------- | ---------------------------------------------------------------------------------- |
| `**0`**                                                       | **5 V → 100 kΩ fixed → analog pin (A1 or A4) → 100 kΩ NTC → GND** (NTC to ground). |
| `**1`** (current defaults in `sensor_pack.ino`)               | **5 V → 100 kΩ NTC → analog pin → 100 kΩ fixed → GND** (NTC toward 5 V).           |


If your bench wiring is **fixed resistor to 5 V** and **NTC to GND** but readings are wrong, set both invert flags to `**0`** and re-flash. If you already use **NTC to 5 V** and **fixed 100 kΩ to GND**, keep invert `**1`**.

Fine-tune with `**ENGINE_TEMP_CAL_OFFSET_C**` / `**AMBIENT_TEMP_CAL_OFFSET_C**` (°C delta after conversion) in the same sketch.

**Note:** The generic ASCII diagram at the top of `sensor_pack.ino` (470 Ω–1 kΩ “pull-down”) describes **fault-detection** style channel ideas; the **thermistor** channels are tuned to the **100 kΩ / 100 kΩ** divider constants above, not that small resistor to ground.

Pin map (sensor MCU), from `sensor_pack.ino`:


| Signal                 | Arduino pin                               |
| ---------------------- | ----------------------------------------- |
| Pot/Fuel sender        | A0                                        |
| Thermistor/Temp sender | A1                                        |
| Pressure sender        | A2                                        |
| Voltage sense          | A3                                        |
| Hall speed pulse       | D2 (interrupt)                            |
| Coil/RPM pulse         | D3 (interrupt)                            |
| ALT indicator          | D4                                        |
| RPM selector bit A     | D5 (pull-up input, strap to GND for LOW)  |
| IGN sense              | D6                                        |
| Fault disable          | D11                                       |
| RPM selector bit B     | D12 (pull-up input, strap to GND for LOW) |


RPM pulses-per-revolution selector (`D5` = A, `D12` = B):

- `A=LOW`, `B=HIGH` -> `4` pulses/rev
- `A=HIGH`, `B=LOW` -> `6` pulses/rev
- `A=HIGH`, `B=HIGH` -> `8` pulses/rev
- `A=LOW`, `B=LOW` -> fallback to default (`8`)

## ATtiny power control unit

Provided ATtiny mapping (kept as source of truth):


| ATtiny pin/function       | Direction vs ATtiny | Connected to                         |
| ------------------------- | ------------------- | ------------------------------------ |
| Pin 5 / PB0 / PI_POWER    | output              | 2N3904 base -> U3 ON/OFF             |
| Pin 6 / PB1 / PI_SIGNAL   | output              | Pi GPIO16 (shutdown notice input)    |
| Pin 7 / ADC1 / IGN_SENSE  | input               | Divider from IGN 12V                 |
| Pin 2 / ADC3 / PI_CONFIRM | input               | Pi GPIO12 (shutdown complete output) |


### ATtiny ISP programming wiring (DIP-8)

Use this mapping when flashing `arduino/sketches/power_control/power_control.ino`:


| Programmer signal | ATtiny85 target pin |
| ----------------- | ------------------- |
| MOSI              | Pin 5 (PB0/MOSI)    |
| MISO              | Pin 6 (PB1/MISO)    |
| SCK               | Pin 7 (PB2/SCK)     |
| RESET             | Pin 1 (RESET)       |
| VCC               | Pin 8 (VCC)         |
| GND               | Pin 4 (GND)         |


Raspberry Pi default wiring used by script (`linuxspi` on SPI0):


| Raspberry Pi pin            | ATtiny85 target pin |
| --------------------------- | ------------------- |
| GPIO10 / pin 19 (SPI0_MOSI) | Pin 5 (PB0/MOSI)    |
| GPIO9 / pin 21 (SPI0_MISO)  | Pin 6 (PB1/MISO)    |
| GPIO11 / pin 23 (SPI0_SCLK) | Pin 7 (PB2/SCK)     |
| GPIO4 / pin 7 (RESET)       | Pin 1 (RESET)       |
| 3V3 / pin 1                 | Pin 8 (VCC)         |
| GND / pin 6                 | Pin 4 (GND)         |


Script:

```bash
bash arduino/scripts/program_power_control_attiny.sh --wiring
```

## Legacy SPI pin overlap (Pi)

SPI display mode to the **Pi** is intentionally dormant in current installer/runtime defaults. SPI0 may still be used for ATtiny programming scripts. **UART4** uses **GPIO 8/9** (TX/RX); do not enable SPI0 on the same pins if UART4 is active.

The **encoder Pico ©2020** uses its **own** SPI0 pins (GP16–GP21, GP18/19); that does not conflict with the Pi’s UART4 on GPIO 8/9.

## Discrepancies corrected

- Removed active reliance on direct Pi GPIO rotary/button inputs in primary configs.
- Replaced MCP23017 controls with dedicated encoder Arduino serial input.
- Standardized FM reset pin usage to GPIO5 in non-SPI configs to avoid overlap with legacy GPIO17 rotary usage.
- **Dual UART (vehicle):** encoder + OLED downlink on **UART4** (`/dev/ttyAMA4`); sensor pack on **UART1** (`/dev/ttyAMA1`). Image hook `20-configure-boot.sh` adds `**dtoverlay=uart1`** and `**dtoverlay=uart4**`.
- **Encoder MCU:** document **Raspberry Pi Pico ©2020** pin map (GP0/GP1 Pi link, SPI OLED on SPI0) alongside legacy **ATmega328P** map.

## ESP32-P4 triple cluster (Pi-less, `firmware/`)

**Sensor pack UART (`sensor_pack.ino`) TX** drives **three ESP32-P4 UART RX** inputs in **parallel** (one transmitter, three receivers): **LEFT** (speedo), **MIDDLE** (tach + clock), **RIGHT** (TSPS). **115200 8N1**, **3.3 V** logic, **common GND**. Do **not** tie multiple ESP **TX** outputs together on the sensor **RX** without a designed combiner; firmware may send `0xA5` keepalive — one board can be receive-only on TX if you omit its `Serial1` TX wire, or rely on continuous sensor_pack frames (~20 Hz) and disable redundant keepalive builds if needed.

Default **ESP32 UART** mapping in PlatformIO: **RX = GPIO5**, **TX = GPIO4** (override with `-DSENSOR_UART_RX` / `-DSENSOR_UART_TX` per board).

**TSPS theme toggle (RIGHT display):** **GPIO6** to GND momentary (internal pull-up), **and/or** capacitive touch (I2C, default address **0x15** on SDA=**7** / SCL=**8**, shared with display PMIC bus). Theme choice is stored in **NVS**.

## ESP32-P4 center ↔ classic ESP32 (Bluetooth A2DP + AVRCP)

Phone audio uses **Bluetooth Classic (A2DP)** on a **second MCU**: a **classic ESP32** (e.g. ESP32-WROOM-32 / ESP32-D0WD; **not** ESP32-S3/C3/S2). Firmware: `arduino/sketches/esp32_audio_bridge/esp32_audio_bridge.ino` (also buildable via `firmware/esp32_classic_bt_audio/`). The center cluster (ESP32-P4) has **no** Classic BT; it reads **track/artist metadata over I2C** from the audio ESP32 at **7-bit address `0x2A`**, same register layout as `integrations.esp32_audio_link` on the Pi.

### I2C (shared display bus on the center board)

Use the **same I2C pins** already used for the Waveshare round display (PMIC **0x45**, touch **0x15** / GT911 **0x5D** or **0x14**): **SDA = GPIO 7**, **SCL = GPIO 8**, **3.3 V**, **GND**. **100 kHz** is fine. Tie the audio ESP32 **SDA/SCL** to that bus; address **0x2A** must not collide with other devices on the wire.

| Signal | ESP32-P4 center (Waveshare) | Classic ESP32 (audio) |
| ------ | --------------------------- | ----------------------- |
| SDA    | **GPIO 7**                  | **GPIO 21** (default `SDA`; set `I2C_SDA_PIN` if your board differs) |
| SCL    | **GPIO 8**                  | **GPIO 22** (default `SCL`; set `I2C_SCL_PIN` if your board differs) |
| GND    | GND                         | GND                     |
| Logic  | 3.3 V                       | 3.3 V                   |

### I2S from audio ESP32 to your DAC (on the audio module, not to the P4)

Default pins in `esp32_audio_bridge.ino`: **BCK = GPIO 14**, **WS/LRCK = GPIO 15**, **DATA = GPIO 22** — **GPIO 22 conflicts with default I2C SCL**. For a real build, assign **non-colliding** I2S pins in the sketch (or move I2C to other pins) and wire to your PCM/I2S DAC.

### Sensor UART (unchanged; not used for Bluetooth)

Sensor pack **UART** to the center display is still **RX = GPIO 5**, **TX = GPIO 4** (**115200 8N1**), same as other ESP32-P4 gauge boards. The Bluetooth audio ESP32 does **not** use this link for A2DP; metadata is **I2C only**.

