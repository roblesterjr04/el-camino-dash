# Console board, 4 keys

A variant of the [console board](../ConsoleBoard/) (CAN node `0x13`) for a housing **111.5 × 38.5 mm outside**: the
same ESP32-S3, the same **SSD1322 256 × 64 OLED** module (100 × 33.5 mm) on the same 2 × 8 socket, CAN, USB-C and
IR header, but **four lit soft keys** (E-Switch TL1240YQ, yellow LED in the plunger) in a column left of the screen
instead of the Back / View keys under it, no separate LEDs, and **two RJ11 jacks** for RetroSound-style knobs
(resistor-ladder encoders, read on the ESP32's ADC) instead of the encoder pigtails.

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
  edge, behind the keys' column), RESET / BOOT, USB-C, the CAN cable, the two RJ11 jacks, the IR header and all the
  passives. The connectors are along the bottom edge, plugs entering from below; left to right seen from the front:
  CAN (J3, under the ESP32), USB-C (J2), Select knob (J4), Volume knob (J5), IR (J6).
- **RJ11 jacks:** Connfly DS1133-S4 (6P4C), 12.5 mm wide and 13.5 mm deep, front face on the bottom edge, centred at
  x = 58.0 and 73.5 mm. They are the tallest parts on the back (check the housing depth behind the board), and the
  housing's bottom wall needs two openings for the plugs.

## Schematic blocks

As on [`ConsoleBoard`](../ConsoleBoard/README.md#schematic-blocks) (MCU, power, USB, CAN, OLED, IR), except:

| Block | Parts | Notes |
| --- | --- | --- |
| Knobs | J4 Select, J5 Volume: RJ11 6P4C jacks; contacts 2 and 5 to GND, 3 and 4 to ADC1 with a 33k pull-up + 10 nF each | See [Knobs](#knobs-rj11) |
| Keys | SW3-SW6 TL1240YQ (keys 1-4, top to bottom) to GND, 10k pull-up + 10 nF on each | Pins 1 / 2 and 3 / 4 are each one contact; 1 / 2 go to GND |
| Key LEDs | The yellow LED in each key (10 mA max, 2.5 V), 100 Ω each from `+3V3` (about 8 mA), Q1 2N7002 on the low side, 100k gate pull-down | GPIO 38 PWM (LEDC) dims all four, as the four separate LEDs were. Square pad = anode (+) |

### Pins

| ESP32 GPIO | Signal | ESP32 GPIO | Signal |
| --- | --- | --- | --- |
| 12 / 11 / 10 | OLED SCK / MOSI / CS | 4 / 7 | Volume knob, jack contacts 3 / 4 (ADC1) |
| 9 / 8 | OLED D/C / RST | 21 / 47 | Key 1 / key 2 (Back / View on `ConsoleBoard`) |
| 5 / 6 | CAN TX / RX | 48 / 13 | Key 3 / key 4 |
| 1 / 2 | Select knob, jack contacts 3 / 4 (ADC1) | 38 | Key LED PWM |
| 19 / 20 | USB D- / D+ | 39 | IR in |

Keys 1 and 2 keep the Back and View pins, so today's console firmware runs on this board with those two keys working;
keys 3 and 4 (GPIO 48 and 13) and the knobs (now analog, see below) need firmware support. GPIO 14-18 are free.

## Knobs (RJ11)

The knobs are RetroSound 258A-style dual controls (inner encoder with push, outer three-position ring), measured in
[gpiozero discussion #1119](https://github.com/gpiozero/gpiozero/discussions/1119). Each has a 4-wire RJ11 lead:

| Jack contact | Wire | Inside the knob |
| --- | --- | --- |
| 2 | Black | Ground |
| 3 | Red | Encoder: to ground through open, 82k, 30k (82k ∥ 47k), 47k, open as it turns one way (the reverse the other way). Open and 30k are the detents, so it is a 2-bit Gray code with two steps per detent |
| 4 | Green | Switches: open (none), 6.8k (outer ring left), 22k (outer ring right), 0 Ω (inner push) |
| 5 | Yellow | Ground (0 Ω to black inside the knob, so never power it) |

Contacts 3 and 4 each have a 33k pull-up to 3.3 V and 10 nF to ground, and go to ADC1 (12 dB attenuation). The
levels:

| Green | Volts | Red | Volts |
| --- | --- | --- | --- |
| push (0 Ω) | 0.00 | 30k (detent) | 1.57 |
| outer left (6.8k) | 0.56 | 47k | 1.94 |
| outer right (22k) | 1.32 | 82k | 2.35 |
| none (open) | 3.30 | open (detent) | 3.30 |

The two lines are wired identically on purpose: a reversed phone cord (the usual kind) swaps black with yellow, which
does nothing, and red with green. The firmware tells them apart by their levels (only the encoder ever reads 1.5 to
2.4 V; only the switches read below 1.4 V) and remembers which is which. Sample each line at 1 kHz or so; the 10 nF
settles in about 1.7 ms (5 τ with 33k), well inside one step of a quickly turned knob.

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
