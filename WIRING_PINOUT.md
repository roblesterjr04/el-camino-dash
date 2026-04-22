# El Camino Dash — Vehicle Wiring Pinout (ESP32 / Sensor / Encoder)

**Canonical vehicle harness** for the Pi-less gauge cluster (`firmware/`). Legacy **Raspberry Pi + HDMI** bench wiring is preserved unchanged in [`WIRING_PINOUT.pi-hdmi-backup.md`](WIRING_PINOUT.pi-hdmi-backup.md).

This document covers only:

- **Sensor pack** (ATmega328P, `arduino/sketches/sensor_pack/sensor_pack.ino`)
- **Encoder controller** (RP2040 Pico `encoder_controller.ino`, or legacy ATmega328P)
- **ESP32-P4** round displays ×3 (speedo, center, TSPS)
- **ESP32-D** (classic Bluetooth audio + AVRCP metadata)

Logic is **3.3 V** unless noted (sensor ATmega may run at **5 V**; level-shift to ESP32 inputs where required).

---

## Sensor pack (ATmega328P)

**UART** to the vehicle: **115200 8N1**, binary frames per [`sensor_packet.h`](firmware/esp32_p4_common/include/sensor_packet.h) / `sensor_pack.ino`.

**Fan-out to gauges:** One **TX** from the sensor MCU may drive **three ESP32-P4 UART RX** in parallel (speedo, center, TSPS). **3.3 V**, common **GND**. Do not tie multiple ESP **TX** lines onto the same sensor **RX** without a designed combiner.

Default ESP32 UART (PlatformIO): **RX = GPIO5**, **TX = GPIO4** (sensor handshake / keepalive).

Analog and digital pin map, thermistor math, and fault behavior: see comments and tables in `arduino/sketches/sensor_pack/sensor_pack.ino`.

---

## Encoder controller

### Raspberry Pi Pico ©2020 (RP2040) — default

Board: original **DIP-40** Pico. Physical **pin numbers** are those printed on the module (USB at bottom; pin 1 = GP0).

`arduino/sketches/encoder_controller/encoder_controller.ino` uses **Serial0** on **GP0 (TX)** / **GP1 (RX)** at **115200** for vehicle UART (rotary, buttons, optional IR tokens, optional OLED downlink). **USB CDC** is optional debug only.

| Function | GPIO | Pico physical pin |
| -------- | ---- | ----------------- |
| UART TX | GP0 | 1 |
| UART RX | GP1 | 2 |
| Menu encoder A/B/push | GP2–GP4 | 4–6 |
| Aux encoder A/B/push | GP5–GP7 | 7–9 |
| Menu left / select | GP8–GP9 | 10–11 |
| SPI0 SCK / MOSI / MISO | GP18 / GP19 / GP16 | 24 / 25 / 21 |
| OLED CS / DC / RST | GP17 / GP20 / GP21 | 22 / 26 / 27 |
| IR demod OUT (optional) | GP22 | 29 |

OLED: SSD1322 256×64 (see sketch for U8g2 profile and `ELC_OLED_*` overrides).

### Legacy ATmega328P

| Control | ATmega328P pin |
| ------- | -------------- |
| Menu / aux encoders, buttons | D2–D9 |
| IR demod OUT | A0 (digital) |
| SPI OLED MOSI/SCK/CS | D11 / D13 / D10 |
| OLED DC / RST | A4 / A5 |

---

## ESP32-P4 — three round displays

### Sensor UART (all three)

- **115200 8N1**, **RX = GPIO5**, **TX = GPIO4** (defaults).
- Connects to **sensor_pack** TX (and optional return path per harness design).

### Display / local I2C (per board)

Typical Waveshare round MIPI kits: **SDA = GPIO7**, **SCL = GPIO8**, **100 kHz**. Devices may include touch (**0x15** CST816 or GT911 **0x5D** / **0x14**), PMIC (**0x45**), etc.

### Cluster sync I2C (inter-board, theme + layout)

Theme and sub-view state are pushed from the **center** display as **I2C master** on a **dedicated second bus** (does not share the display/touch SDA/SCL pair).

| Role | GPIO (default) | Notes |
| ---- | -------------- | ----- |
| Cluster SDA | **47** | Tie **SDA** together: center (master), speedo (slave), TSPS (slave), optional ESP32-D |
| Cluster SCL | **48** | Tie **SCL** together; **3.3 V**, **GND**, **pull-ups** on the segment |

Override with `-DCLUSTER_I2C_SDA=…` / `-DCLUSTER_I2C_SCL=…` in PlatformIO if your PCB uses other pins.

**7-bit addresses** (do not collide with touch/audio on the display bus):

| Device | Address | Notes |
| ------ | ------- | ----- |
| Speedo (slave) | **0x3C** | Receives theme bytes from center |
| TSPS (slave) | **0x3D** | Receives theme + TSPS classic variant |
| ESP32-D theme mirror (optional slave) | **0x3E** | Optional NVS/theme sync |
| Classic BT metadata (on center’s **display** bus) | **0x2A** | AVRCP text; not on cluster bus |

Registers (1 byte data each): **0x01** = theme (0 Classic, 1 Fallout); **0x02** = center sub-view (0 Tach, 1 Clock, 2 Media); **0x03** = TSPS classic variant (0 classic_1, 1 classic_2).

**TSPS:** Local **Wire** on GPIO **7/8** stays **master** for touch/PMIC; **cluster slave** uses **`TwoWire(1)`** on GPIO **47/48**.

**Speedo:** No touch in stock firmware; **cluster slave** on **47/48** only.

**Center:** **`TwoWire(1)`** as **master** on **47/48** to broadcast to slaves after long-press gestures.

### Ambient illumination (all three round displays)

**One digital input per board**, internal pull-up: **LOW** → dim backlight, **HIGH** (open) → full. Tie the same vehicle signal to **GPIO 24** on each ESP32-P4 module (silk **“24”** on the Waveshare 2×20 header). **GPIO 23** is not labeled on that header and is not used as the default harness pin. Do **not** use **GPIO 15** (SDIO to the C6 WiFi coprocessor on the 3.4C board). Override in firmware: `-DELC_ILLUMINATION_GPIO=…` / `-1` to disable.

### TSPS — bench theme GPIO (optional)

**GPIO6** to GND (internal pull-up): legacy bench toggle; long-press touch is preferred for classic variant when theme is Classic.

---

## ESP32-D — classic Bluetooth audio (A2DP + AVRCP)

Use **`arduino/sketches/esp32_audio_bridge/esp32_audio_bridge.ino`** (or `firmware/esp32_classic_bt_audio/`). The P4 center has no Classic BT; it reads **now playing** text over **I2C** at **0x2A** on the **display** bus (**GPIO7/8** on the center module), same register layout as `integrations.esp32_audio_link` in the Python stack.

| Signal | Center ESP32-P4 | Audio ESP32-D |
| ------ | --------------- | ------------- |
| I2C SDA | GPIO7 | GPIO21 (default; set `I2C_SDA_PIN` if needed) |
| I2C SCL | GPIO8 | GPIO22 (default) |
| GND | GND | GND |

**I2S** from the audio module to your DAC: assign **non-colliding** BCK/WS/DATA pins in the audio sketch (default I2S pins may conflict with I2C on some modules).

Optional: connect ESP32-D to the **cluster** bus (**47/48**) as slave **0x3E** to mirror the global **theme** byte in NVS (see firmware).

---

## Discrepancies / notes

- **Dual UART** on a Raspberry Pi (encoder vs sensor) is described only in the **backup** pinout, not here.
- SPI0 on a Pi was used for ATtiny programming in older installs; vehicle **ESP32** builds do not require it.
