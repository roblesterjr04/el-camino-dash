# Console board, 4 keys

A variant of the [console board](../ConsoleBoard/) (CAN node `0x13`) for a housing **111.5 × 38.5 mm outside**: the
same ESP32-S3, the same **SSD1322 256 × 64 OLED** module (100 × 33.5 mm) on the same 2 × 8 socket, CAN, USB-C,
encoder pigtails and IR header, but **four lit soft keys** (E-Switch TL1240YQ, yellow LED in the plunger) in a column
left of the screen instead of the Back / View keys under it, and no separate LEDs.

| Files | |
| --- | --- |
| `ConsoleBoard4Key.kicad_sch` / `.kicad_pcb` / `.kicad_pro` | The board: 108.0 × 35.0 mm, 4 layers, parts on both sides. KiCad 10 |
| `ConsoleBoard4Key.pretty` | The TL1240YQ footprint (E-Switch drawing P010363 rev E). Everything else is stock KiCad |
| `generate/` | `gen_footprints.py`, `gen_sch.py`, `gen_pcb.py`, `drc.py` (see [Regenerating](#regenerating)) |
| `ConsoleBoard4Key_BOM.csv`, `fab/` | BOM, Gerbers / drill, placement and centroid, PCBWay BOM |

## Mechanics

```
                                108.0 mm
   +--------+--o---------------------------------------------------o--+  ---
   | (KEY1) |                                                       :: |
   | (KEY2) |                  OLED module 100 x 33.5                :: |  35.0   the module covers the board top to bottom
   | (KEY3) |                  (on standoffs, o = M2 holes)          :: |         (0.75 mm margin) and is flush with the
   | (KEY4) |                                                       :: |         right edge; J1 (::) as on ConsoleBoard
   +--------+--o---------------------------------------------------o--+  ---
     8.0 mm strip: four TL1240YQ at x = 4.25, 8.5 mm pitch, centred top to bottom
```

- **Housing:** the board is 108.0 × 35.0 mm, so the 111.5 × 38.5 housing has 1.5 mm walls with 0.25 mm clearance
  all round. All four layers carry copper to 0.2 mm from the edge (the default rules), and the module's mounting holes
  are 3.5 mm from the top and bottom edges: the standoffs (M2, about 4 mm across the flats) end about 1.5 mm from the
  board edge.
- **Module:** exactly as on `ConsoleBoard` (`generate/gen_pcb.py` keeps the same parameters): holes 94 × 28 mm centre to
  centre, J1 a female 2 × 8 socket on the right-hand holes' line shifted 0.5 mm toward the edge, module pin *n* on
  socket pad 17 − *n*. Everything is moved 8 mm right (`MX`) and 0.75 mm down (`MY`).
- **Keys:** four TL1240YQ (6 × 6 mm body, through hole), keys 1-4 top to bottom at x = 4.25 mm, y = 4.75, 13.25, 21.75
  and 30.25 mm. The legs land 1.0 and 7.5 mm from the left edge, so the right-hand pins sit just under the module's
  edge (which is about 8.5 mm up on its standoffs). The plunger top is 7.2 mm above the board, below the module's face:
  the housing needs **key caps** (or extended plungers) to reach its front. The LED shines through the clear plunger, so
  a translucent cap lights up.
- **Labels:** the screen is 19.2 mm tall and the keys span 25.5 mm, so the firmware draws the four labels as a list
  down the screen's left edge (top key = top label), not level with each key.
- **Front** (module side): J1, the four M2 holes and the four keys. **Back:** the ESP32 module (antenna on the top
  edge, behind the keys' column), RESET / BOOT, USB-C, the CAN cable, both encoder connectors, the IR header and all
  the passives. The cable connectors are along the bottom edge, with their captions on one line between them and the
  ESP32.

## Schematic blocks

As on [`ConsoleBoard`](../ConsoleBoard/README.md#schematic-blocks) (MCU, power, USB, CAN, OLED, encoders, IR), except:

| Block | Parts | Notes |
| --- | --- | --- |
| Keys | SW3-SW6 TL1240YQ (keys 1-4, top to bottom) to GND, 10k pull-up + 10 nF on each | Pins 1 / 2 and 3 / 4 are each one contact; 1 / 2 go to GND |
| Key LEDs | The yellow LED in each key (10 mA max, 2.5 V), 100 Ω each from `+3V3` (about 8 mA), Q1 2N7002 on the low side, 100k gate pull-down | GPIO 38 PWM (LEDC) dims all four, as the four separate LEDs were. Square pad = anode (+) |

### Pins

| ESP32 GPIO | Signal | ESP32 GPIO | Signal |
| --- | --- | --- | --- |
| 12 / 11 / 10 | OLED SCK / MOSI / CS | 18 / 7 / 14 | Volume A / B / push |
| 9 / 8 | OLED D/C / RST | 21 / 47 | Key 1 / key 2 (Back / View on `ConsoleBoard`) |
| 5 / 6 | CAN TX / RX | 48 / 13 | Key 3 / key 4 |
| 15 / 16 / 17 | Select A / B / push | 38 | Key LED PWM |
| 19 / 20 | USB D- / D+ | 39 | IR in |

Keys 1 and 2 keep the Back and View pins, so today's console firmware runs on this board with those two keys working;
keys 3 and 4 (GPIO 48 and 13) need firmware support.

J3 (cable to the DigiDash board): 1 +5 V, 2 CANH, 3 CANL, 4 GND (the cable-port order, see
`docs/hardware/accessory_ports.md`).

## Regenerating

KiCad 10 with its stock libraries, numpy in KiCad's Python (`$KIPY -m pip install --user numpy`) and Freerouting
1.9.0 (Java 17 or newer, opens its window while it routes):

```bash
KIPY=/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3
python3 generate/gen_footprints.py                             # ConsoleBoard4Key.pretty
python3 generate/gen_sch.py                                    # also writes generate/refs.json
JAVA=/opt/homebrew/opt/openjdk@17/bin/java $KIPY generate/gen_pcb.py --route freerouting.jar
python3 generate/drc.py
python3 ../tools/fab_export.py ConsoleBoard4Key
```
