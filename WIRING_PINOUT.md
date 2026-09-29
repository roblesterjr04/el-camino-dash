# El Camino Dash — Vehicle Wiring Pinout (ESP32 / Sensor / Encoder)

**Canonical vehicle harness** for the Pi-less gauge cluster (`firmware/`). Legacy **Raspberry Pi + HDMI** bench wiring is preserved unchanged in [`WIRING_PINOUT.pi-hdmi-backup.md`](WIRING_PINOUT.pi-hdmi-backup.md).

This document covers only:

- **Sensor pack** (ATmega328P, `arduino/sketches/sensor_pack/sensor_pack.ino`)
- **Encoder controller** (RP2040 Pico `encoder_controller.ino`, or legacy ATmega328P). **Not yet connected to any ESP32 firmware.**
- **ESP32-P4** round displays ×3 (speedo, center, TSPS)
- **ESP32-D** (classic Bluetooth audio + AVRCP metadata)

Logic is **3.3 V** unless noted (sensor ATmega may run at **5 V**; level-shift to ESP32 inputs where required).

---

## Sensor pack (ATmega328P)

**UART** to the vehicle: **115200 8N1**, binary frames per [`sensor_packet.h`](firmware/esp32_p4_common/include/sensor_packet.h) / `sensor_pack.ino`.

**Fan-out to gauges:** One **TX** from the sensor MCU may drive **three ESP32-P4 UART RX** in parallel (speedo, center, TSPS). **3.3 V**, common **GND**.

**Return path (bench harness only):** wire only the **center's** GPIO4 (TX) to the sensor MCU RX. The center is the only board that sends the 0xA5 keepalive; the left and right screens open the UART receive-only. On the DigiDash board GPIO4 is the screen **position strap** instead (below), and J1 leaves it open.

Default ESP32 UART (PlatformIO): **RX = GPIO5**, **TX = GPIO4**.

**Level shifting:** a 5 V ATmega TX into 3.3 V ESP32 RX needs a divider or level shifter.

**D11 (demo mode):** HIGH makes the sensor pack send a slow fake sweep instead of real data. It is a plain input with no pull resistor — tie it to GND in the vehicle harness (or fit a pull-down); strap HIGH only on the bench. The DigiDash board has a 10 kΩ pull-down (R17).

**DigiDash board inputs:** D6 ignition sense = ignition 12 V (J7 pin 3) through 22 kΩ / 10 kΩ; D4 alternator "L" =
J3 pin 4 through 22 kΩ / 10 kΩ; D10 (charge-channel power monitor) pulled up to 5 V (that channel has no supply of its
own; low would report a sensor-power fault); A1 pull-down 100 kΩ (100k NTC); A2 pull-down 1 kΩ (match your sender).

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

### Center-only peripherals

| Function | GPIO | Notes |
| -------- | ---- | ----- |
| GPS NMEA RX (GPS TX →) | 2 | UART2, 9600 8N1 (NEO-6M default; `GPS_UART_BAUD`) |
| GPS TX (→ GPS RX) | 3 | optional |
| Backlight PWM (3.4C) | 26 | `TSPS_BACKLIGHT_PIN`, active-low |
| TF card (SDIO, 3.4C) | 39–44 | on-board slot: `elcamino/settings.json` on the card (Wi-Fi password stored in plain text) |

### Display / local I2C (per board)

Typical Waveshare round MIPI kits: **SDA = GPIO7**, **SCL = GPIO8**, **100 kHz**. Devices may include touch (**0x15** CST816 or GT911 **0x5D** / **0x14**), PMIC (**0x45**), etc.

### Cluster sync I2C (inter-board, theme + layout)

Theme and sub-view state are pushed from the **center** display as **I2C master** on a **dedicated second bus** (does not share the display/touch SDA/SCL pair).

| Role | GPIO (default) | Notes |
| ---- | -------------- | ----- |
| Cluster SDA | **47** | Tie **SDA** together: center (master), speedo (slave), TSPS (slave), optional ESP32-D |
| Cluster SCL | **48** | Tie **SCL** together; **3.3 V**, **GND**, **pull-ups** on the segment |

Override with `-DCLUSTER_I2C_SDA=…` / `-DCLUSTER_I2C_SCL=…` in PlatformIO if your PCB uses other pins.

**DigiDash carrier board** (`DigiDash/`): nets `CL_SDA` / `CL_SCL` join GPIO47 / GPIO48 on the three screen headers
(J1 center, J4 right, J5 left), with 4.7 kΩ pull-ups (R15/R16) to the center's 3V3 pin. The screens plug face-down
onto the headers, so each header row is mirrored relative to the Waveshare pin order; the symbol labels account for that.

**Screen position strap** (one firmware for all three screens, `firmware/esp32_p4_display`): header **pin 17 = GPIO4**
is read once at boot, first with the internal pull-up, then with the pull-down:

| Header | Strap | Reads | Runs as |
| --- | --- | --- | --- |
| J5 | R37 1 kΩ from pin 17 to GND | low / low | left (speedometer), CAN node 0x02, I2C slave 0x3C |
| J1 | pin 17 open | follows the pull | center, CAN node 0x01 |
| J4 | R38 1 kΩ from pin 17 to that screen's own 3V3 (J4 pin 18) | high / high | right (fuel / temp / oil / amp), CAN node 0x03, I2C slave 0x3D |

The 1 kΩ keeps a board that drives GPIO4 (older firmware used it as the UART TX) from shorting the strap. The right
strap uses the right screen's own 3V3, not the center's, so it still reads right with the center unplugged. A board
off the DigiDash board reads "open" (center); `role left|center|right` on its serial console overrides that on a bench.

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

### CAN bus (three displays + accessory ports)

Full spec: [`docs/protocols/can_bus.md`](docs/protocols/can_bus.md). One **MCP2562-E/P** (DIP-8) per screen on the
DigiDash board: VDD 5 V, Vio 3.3 V, STBY to GND, 10 kΩ TXD pull-up to 3.3 V. **120 Ω termination only at the left
(R35 on U5) and right (R36 on U10) screens.** Route CANH/CANL as a pair from U5 past the accessory ports and U9 to
U10. **J16** is a CANH / CANL / GND tap for a USB-CAN adapter; D2 (NUP2105L) is the bus ESD clamp.

| Node | Transceiver | CAN TX | CAN RX |
| ---- | ----------- | ------ | ------ |
| Left screen (J5) | U5 | GPIO34 (header pin 31) | GPIO36 (header pin 23) |
| Center screen (J1) | U9 | GPIO34 (header pin 31) | GPIO36 (header pin 23) |
| Right screen (J4) | U10 | GPIO34 (header pin 31) | GPIO36 (header pin 23) |

**Accessory ports** ([`docs/hardware/accessory_ports.md`](docs/hardware/accessory_ports.md)): four 2×5 header
positions **J18–J21** on the back (1, 2 +5 V; 3, 4, 7 GND; 5 CANH; 6 CANL; 8 +12 V switched; 9, 10 reserved; asymmetric
M3 standoff holes) and two JST GH cable ports **J17 / J22** (1 +5 V, 2 CANH, 3 CANL, 4 GND). Every 5 V and 12 V feed
has its own 0.5 A PTC (F1–F10). Carriers bring their own MCU and MCP2562 and never terminate the bus.

**Console cable (cable port J17, JST GH 4-pin):** 1 = +5 V, 2 = CANH, 3 = CANL, 4 = GND (CANH/CANL twisted). The console
(OLED + knobs board, ESP32-S3) is an unterminated branch: R35/R36 stay the only terminators, so the bus works with it unplugged,
and the console board must not fit a 120 Ω. Keep the cable short (under ~30 cm at 500 kbit/s). Details in
[`docs/protocols/can_bus.md`](docs/protocols/can_bus.md#console-cable-j17).

### Backup camera (center): TVP5150 video decoder module

Composite video → TVP5150AM1 module (2×10 header, OV7670-style order) → 8-bit BT.656 into the center's camera (DVP)
input. On the DigiDash board the module plugs into **J13** (2×10 socket, back side; the module hangs toward the right
board edge so its RCA jack is reachable). The module's pin 1 (VCC) sits at the **far end** of the socket: module pin
*p* lands on J13 pad 20 − *p* (odd) / 22 − *p* (even), i.e. VCC and GND on pads 19/20. Control is I2C on the **cluster bus** (GPIO47/48; TVP5150 at 0x5C/0x5D —
kept off the touch bus, where the GT911 can also sit at 0x5D). Module VCC is the center's 3.3 V.

| Module pin | Signal | Center GPIO | | Module pin | Signal | Center GPIO |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | VCC 3.3 V | — | | 2 | GND | — |
| 3 | SCL | 48 | | 4 | SDA | 47 |
| 5 | V (VSYNC) | 50 | | 6 | H (HSYNC) | 51 |
| 7 | PCLK (unlabeled on the module) | 49 | | 8 | — (unlabeled) | — |
| 9 | D7 | 28 | | 10 | D6 | 32 |
| 11 | D5 | 31 | | 12 | D4 | 30 |
| 13 | D3 | 29 | | 14 | D2 | 22 |
| 15 | D1 | 21 | | 16 | D0 | 20 |
| 17 | — (unlabeled) | 52 | | 18 | FID | 46 |
| 19 | INT | — | | 20 | IO | — |

**Reverse trigger:** reverse-lamp 12 V (J9 pin 1) → R8/Q3 → **GPIO25** on the center (active low, internal pull-up).
GPIO25 is also a USB pin on the P4, so it carries the slow reverse signal rather than video data. GPIO34–36 are boot
straps and are not used for the camera.

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

On the DigiDash board the ESP32-D sits on an **audio carrier** on one of the header positions and talks to the center
over the **CAN bus** only (TX GPIO32, RX GPIO33 through the carrier's MCP2562; GPIO13 / GPIO23 open = node 0x10):
now-playing, transport and album art. The I2C wiring above is for **bench** setups without transceivers: it is used
only by the center's `*-bench` envs and the bridge's `esp32dev_bench` env.

**I2S amplifiers** (MAX98357A, `esp32dev_external_i2s` build) live on the audio carrier: DIN = **GPIO27**,
BCLK = **GPIO14**, LRC = **GPIO25**, GAIN can float (9 dB). Each amp is mono; SD selects the channel (tied to VIN through
a jumper = left, through 680 kΩ = right, open = L+R mix). Power the amps from the port's +12 V through a 5 V buck on the
carrier (at full volume they draw more than the port's 0.5 A at 5 V). The DevKit runs from the port's +5 V; its own 3V3
pin stays unconnected.

---

## Discrepancies / notes

- **Dual UART** on a Raspberry Pi (encoder vs sensor) is described only in the **backup** pinout, not here.
- SPI0 on a Pi was used for ATtiny programming in older installs; vehicle **ESP32** builds do not require it.
