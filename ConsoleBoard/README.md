# Console board

The console's own board (CAN node `0x13`): a bare **ESP32-S3-WROOM-1-N8** (no dev board) with the parts it needs, a
female 2 × 8 socket the 100 × 33.5 mm **SSD1322 256 × 64 OLED** module plugs into, **Back** and **View** keys, four yellow LEDs (to match the yellow OLED), and top-entry
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
   |        OLED module         :: |   its 2 x 8 pins plug into the socket J1 on the right edge (:)
   |                            :: |   33.5
   +--o------------------------o--+  ---   <- module's bottom edge
   |   *[ BACK ]*        *[ VIEW ]*  |  |   * = LED
   |     x=25            x=75      |  16.5   the board reaches 16.5 mm past the module (15 to 18 mm wanted)
   | J3 J4     USB      J5  J6     |  |     connectors along the bottom edge on the back (top entry, USB side entry)
   +-------------------------------+  ---
```

- **Keys:** PTS645 tact switches (click feel), centred at x = 25 and 75 mm and y = 37.5 mm, to sit behind the two
  facia openings (the facia is drawn at the board's 2:1 proportions: 100 × 50 mm). Four LEDs flank them, 9.5 mm either side of each key.
- **Front** (module side): J1, the four M2 holes, the two keys and the four LEDs with their resistors and MOSFET. Everything
  else is on the back, so the standoff height is set by the socket: J1 is a female socket (about 8.5 mm tall), so the module's pins plug into it and the standoffs should hold the module about that far above the board (check the length of the module's pins).
- **Back:** the ESP32 module (antenna on the top edge), USB-C, the CAN cable, both encoder connectors, the IR header,
  RESET / BOOT and all the passives. J3-J6 are top entry (vertical), so their cables plug straight into the back of the
  board inside the housing; only the USB-C opens past the bottom edge.
- **Module dimensions are parameters** at the top of `generate/gen_pcb.py`: the 100 × 33.5 mm outline and the four M2
  holes, 94 × 28 mm centre to centre (measured), centred on the module (3 mm in from the sides, 2.75 mm from the top
  and bottom). J1's centre line, between its two pin columns, is on the right-hand holes' line, shifted 0.5 mm toward
  the edge (`HDR_SHIFT`): columns at x = 96.23 and 98.77 mm. It is centred top to bottom between those two holes.

## Schematic blocks

| Block | Parts | Notes |
| --- | --- | --- |
| MCU | U1 ESP32-S3-WROOM-1-N8, 10k + 1 µF on EN, RESET (SW1), BOOT (SW2), 22 µF + 10 µF + 100 nF on 3V3 | No PSRAM, so GPIO 35-37 stay free. Native USB on GPIO 19 / 20 |
| Power | J3 pin 1 +5 V or USB VBUS, ORed by D1 / D2 (B5819W) into `+5V`; U3 AP7361C-33E (1 A, 0.36 V dropout, SOT-223) | The board runs from the cable in the car; USB-C is for programming and bench power. SOT-223 because U3 carries the ESP32, the OLED and the LEDs (up to about 0.6 W with the OLED all white) |
| CAN | U2 MCP2562-E/SN (VDD 5 V straight from J3, ahead of the OR diode, so it stays above its 4.5 V minimum; VIO 3.3 V, STBY low), 1k on RXD, D5 NUP2105L on the pair | Never terminated: the DigiDash board owns both 120 Ω |
| OLED | J1, 2 × 8, SPI on the ESP32's FSPI pins | See the pin table below |
| LEDs | D3, D4, D6, D7 yellow 0603 (about 590 nm, to match the OLED) (two per key), 100 Ω each from `+3V3` (about 13 mA each), Q1 2N7002 on the low side, 100k gate pull-down | Same 3.3 V rail as the OLED. GPIO 38 PWM (LEDC) dims all four |
| Keys, encoders | SW3 BACK, SW4 VIEW, J4 Select and J5 Volume (JST PH 4-pin top entry, B4B-PH-K, GND / A / B / SW), 10k pull-up + 10 nF on every line | The encoders are cabled to J4 / J5: the board has no encoder footprints |
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

The pin order below was **measured on the module** (a 2 × 8 header: odd pins in the row marked "1", even pins in the row
marked "2"), and the module works this way on the bench. Only the seven SPI pins are wired; the parallel-bus pins stay open,
as on the bench. Orientation, checked on the module: its even row (2 … 16) faces the board's outer (right) edge with pin 2
at the bottom (toward the keys), so module pin *n* sits on socket pad 17 − *n* (`SOCKET_NETS` in `generate/gen_sch.py`).

| Pin | Module | Board net | Pin | Module | Board net |
| --- | --- | --- | --- | --- | --- |
| 1 | GND | GND | 14 | D/C | OLED_DC |
| 2 | VCC | +3V3 | 15 | RES | OLED_RST |
| 4 | D0 (CLK) | OLED_SCK | 16 | CS | OLED_CS (100k to 3V3) |
| 5 | D1 (DIN) | OLED_MOSI | 3, 6-13 | NC, D2-D7, E/RD, R/W | open |

The module must be set for 4-wire SPI (its BS resistors), as it is in the firmware today.

J3 (cable to the DigiDash board, JST PH 4-pin top entry, B4B-PH-K): 1 +5 V, 2 CANH, 3 CANL, 4 GND (the cable-port order, see
`docs/hardware/accessory_ports.md`).

## Firmware notes

- CAN: TWAI on GPIO 5 (TX) / 6 (RX), node `0x13`, 500 kbit/s, as in `docs/protocols/can_bus.md`.
- OLED: the firmware's own SSD1322 driver (`firmware/esp32s3_console/src/oled_ssd1322.cpp`, NHD init table, whole
  16-grey frames turned 180°) on SPI with CS 10, DC 9, RST 8. The LEDs follow the same brightness through `ledcWrite` on GPIO 38.
- Keys and encoders are active-low with external pull-ups; use the internal pull-ups only as a fallback.
- Firmware: [`firmware/esp32s3_console/`](../firmware/esp32s3_console/) (soft keys, phone control, bench serial console).

## Regenerating

KiCad 10 with its stock libraries, numpy in KiCad's Python (`$KIPY -m pip install --user numpy`) and Freerouting
(Java 17 or newer, opens its window while it routes):

```bash
KIPY=/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3
python3 generate/gen_sch.py                                    # also writes generate/refs.json
JAVA=/opt/homebrew/opt/openjdk@17/bin/java $KIPY generate/gen_pcb.py --route freerouting.jar
python3 generate/drc.py
```
