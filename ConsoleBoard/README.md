# Console board

The console's own board (CAN node `0x13`): a bare **ESP32-S3-WROOM-1-N8** (no dev board) with the parts it needs, a
2 × 8 header the 100 × 33.5 mm **SSD1322 256 × 64 OLED** module solders to, **Back** and **View** keys, two orange LEDs, and pigtail
connectors for the **Select** and **Volume** encoders. One 4-wire cable goes to a DigiDash cable port.

| Files | |
| --- | --- |
| `ConsoleBoard.kicad_sch` / `.kicad_pcb` / `.kicad_pro` | The board: 100.0 × 50.0 mm, 4 layers, parts on both sides. KiCad 10 |
| `generate/` | Scripts that produced the files (`gen_sch.py`, `gen_pcb.py`, `drc.py`). Everything comes from stock KiCad libraries |

## Mechanics

```
              100.0 mm
   +--o------------------------o--+  ---
   |                            :  |   the module (100 x 33.5) sits above the board on four standoffs (o)
   |        OLED module         :: |   its 2 x 8 pins solder into J1 on the right edge (:)
   |                            :: |   33.5
   +--o------------------------o--+  ---   <- module's bottom edge
   |     [ BACK ]  LED LED [ VIEW ]|  |
   |     x=25            x=75      |  16.5   the board reaches 16.5 mm past the module (15 to 18 mm wanted)
   | J3 J4     USB      J5  J6     |  |     connectors along the bottom edge on the back
   +-------------------------------+  ---
```

- **Keys:** PTS645 tact switches (click feel), centred at x = 25 and 75 mm and y = 37.5 mm, to sit behind the two
  facia openings (the facia is drawn at the board's 2:1 proportions: 100 × 50 mm). The two LEDs sit between them, 8 mm
  either side of the centre.
- **Front** (module side): J1, the four M2 holes, the two keys and the LEDs with their resistors and MOSFET. Everything
  else is on the back, so the standoff gap only has to clear J1's 2.5 mm plastic and the LED parts.
- **Back:** the ESP32 module (antenna on the top edge), USB-C, the CAN cable, both encoder connectors, the IR header,
  RESET / BOOT and all the passives.
- **Module dimensions are parameters** at the top of `generate/gen_pcb.py`. The 100 × 33.5 mm outline is measured from
  the module's drawing; **the header's distance from the right edge (`HDR_FROM_RIGHT`) and the hole inset
  (`HOLE_INSET`) are estimates from a picture**: measure them, edit, and regenerate (see below).

## Schematic blocks

| Block | Parts | Notes |
| --- | --- | --- |
| MCU | U1 ESP32-S3-WROOM-1-N8, 10k + 1 µF on EN, RESET (SW1), BOOT (SW2), 22 µF + 10 µF + 100 nF on 3V3 | No PSRAM, so GPIO 35-37 stay free. Native USB on GPIO 19 / 20 |
| Power | J3 pin 1 +5 V or USB VBUS, ORed by D1 / D2 (B5819W) into `+5V`; U3 AP2112K-3.3 | The board runs from the cable in the car; USB-C is for programming and bench power |
| CAN | U2 MCP2562-E/SN (VDD 5 V, VIO 3.3 V, STBY low), 1k on RXD, D5 NUP2105L on the pair | Never terminated: the DigiDash board owns both 120 Ω |
| OLED | J1, 2 × 8, SPI on the ESP32's FSPI pins | See the pin table below |
| LEDs | D3 / D4 orange 0603, 100 Ω each from `+3V3` (about 13 mA each), Q1 2N7002 on the low side, 100k gate pull-down | Same 3.3 V rail as the OLED. GPIO 38 PWM (LEDC) dims both |
| Keys, encoders | SW3 BACK, SW4 VIEW, J4 Select and J5 Volume (JST PH 4-pin, GND / A / B / SW), 10k pull-up + 10 nF on every line | Pigtail the encoders: the board has no encoder footprints |
| Extras | J6 IR receiver header (GND, 3.3 V, OUT), M2 holes H1-H4 tied to GND | The IR header is for the existing `encoder_controller` IR feature |

### Pins

| ESP32 GPIO | Signal | ESP32 GPIO | Signal |
| --- | --- | --- | --- |
| 12 / 11 / 10 | OLED SCK / MOSI / CS | 18 / 7 / 14 | Volume A / B / push |
| 9 / 8 | OLED D/C / RST | 21 | BACK |
| 5 / 6 | CAN TX / RX | 47 | VIEW |
| 15 / 16 / 17 | Select A / B / push | 38 | LED PWM |
| 19 / 20 | USB D- / D+ | 39 | IR in |

Strapping pins 0 (BOOT), 3, 45, 46 are not otherwise used.

### J1 (the OLED header) and the cable

**The pin order below is from a forum listing of this kind of module, not from a datasheet. Check it against your
module before ordering the board** (and which corner is pin 1: the board has pin 1 top-left of the pair, odd pins in
the inner column). If the module's order differs, edit `OLED_PINS` in `generate/gen_sch.py` and regenerate.

| Pin | Module | Board net | Pin | Module | Board net |
| --- | --- | --- | --- | --- | --- |
| 1 | VCC (3.3-5 V) | +3V3 | 9 | D0 (SCLK) | OLED_SCK |
| 2, 3 | GND | GND | 10 | D1 (SDIN) | OLED_MOSI |
| 4 | RES | OLED_RST | 11 | D2 | open |
| 5 | CS | OLED_CS (100k to 3V3) | 12-16 | D3-D7 | GND |
| 6 | D/C | OLED_DC | | | |
| 7, 8 | R/W, E/RD | GND | | | |

The module must be set for 4-wire SPI (its BS resistors), as it is in the firmware today.

J3 (cable to the DigiDash board): 1 +5 V, 2 CANH, 3 CANL, 4 GND (the cable-port order, see
`docs/hardware/accessory_ports.md`).

## Firmware notes

- CAN: TWAI on GPIO 5 (TX) / 6 (RX), node `0x13`, 500 kbit/s, as in `docs/protocols/can_bus.md`.
- OLED: U8g2 `U8G2_SSD1322_NHD_256X64_F_4W_HW_SPI` (or the ZJY variant) with CS 10, DC 9, RST 8. Dim it with the
  contrast command and drive the LEDs from the same brightness value with `ledcWrite` on GPIO 38.
- Keys and encoders are active-low with external pull-ups; use the internal pull-ups only as a fallback.

## Regenerating

KiCad 10 with its stock libraries, numpy in KiCad's Python (`$KIPY -m pip install --user numpy`) and Freerouting
(Java 17 or newer, opens its window while it routes):

```bash
KIPY=/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3
python3 generate/gen_sch.py                                    # also writes generate/refs.json
JAVA=/opt/homebrew/opt/openjdk@17/bin/java $KIPY generate/gen_pcb.py --route freerouting.jar
python3 generate/drc.py
```
