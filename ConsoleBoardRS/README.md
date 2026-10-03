# Console board, RetroSound knobs

A variant of the [console board](../ConsoleBoard/) (CAN node `0x13`) for a housing **111.5 × 38.5 mm outside**, with
the controls on two **RetroSound 258A-style knobs** (each an encoder with push, inside a ring that pushes left or
right) on RJ11 cables instead of keys on the board. Same ESP32-S3, the same **SSD1322 256 × 64 OLED** module
(100 × 33.5 mm) on the same 2 × 8 socket, CAN, USB-C and IR header; no keys and no key LEDs.

| Files | |
| --- | --- |
| `ConsoleBoardRS.kicad_sch` / `.kicad_pcb` / `.kicad_pro` | The board: 108.0 × 35.0 mm, 4 layers, parts on both sides. KiCad 10 |
| `ConsoleBoardRS.pretty` | The MJBA-640 RJ11 footprint (Wayconn drawing MJBA-XXXXXX rev A). Everything else is stock KiCad |
| `generate/` | `gen_footprints.py`, `gen_sch.py`, `gen_pcb.py`, `drc.py` (see [Regenerating](#regenerating)) |
| `ConsoleBoardRS_BOM.csv`, `fab/` | BOM, Gerbers / drill, placement and centroid, PCBWay BOM |

## Mechanics

```
                                 108.0 mm
   +----o------------------------------------------------------o----+  ---   module flush with the top edge
   |                                                          ::    |
   |                 OLED module 100 x 33.5                   ::    |  35.0  centred left to right (4 mm each side)
   |                 (on standoffs, o = M2 holes)             ::    |
   +----o------------------------------------------------------o----+  ---   1.5 mm of board below the module
```

- **Housing:** the board is 108.0 × 35.0 mm, so the 111.5 × 38.5 housing has 1.5 mm walls with 0.25 mm clearance
  all round. The module is centred left to right and **flush with the board's top edge**, as high as the housing
  allows: its top is 1.75 mm below the housing's top outside edge and its bottom 3.25 mm above the bottom outside edge,
  which gives the bezel its text band below the screen. The module's top holes are 2.75 mm from the board's top
  edge, so the M2 standoffs (about 4 mm across the flats) end about 0.5 mm from it.
- **Module:** exactly as on `ConsoleBoard` (`generate/gen_pcb.py` keeps the same parameters): holes 94 × 28 mm centre to
  centre, J1 a female 2 × 8 socket on the right-hand holes' line shifted 0.5 mm toward the edge, module pin *n* on
  socket pad 17 − *n*. Everything is moved 4 mm right (`MX`); `MY` = 0.
- **Front** (module side): J1 and the four M2 holes, nothing else. **Back:** the ESP32 module (antenna on the top
  edge), RESET / BOOT, USB-C, the CAN cable, the two RJ11 jacks, the IR header and all the passives. CAN (J3, under
  the ESP32), USB-C (J2) and the IR header (J6) are on the bottom edge; the two knob jacks (J4 Select, J5 Volume)
  face out of the back.
- **CAN connector:** fit a **vertical** JST PH (B4B-PH-K) on J3's footprint: it has the same four holes and pads as the
  side-entry S4B-PH-K the footprint is named for, so the plug goes in from behind the board like the knob plugs.
- **RJ11 jacks:** Wayconn MJBA-640 (6P4C, top entry), 13.46 × 16.38 mm on the board and 16.5 mm tall, on the back with
  their openings facing away from the board: the plugs go in from behind, straight toward the board. Centre lines at
  x = 58.0 and 74.0 mm (seen from the front); the bodies end 1 mm above the bottom edge. The drawing does not tie the
  body to the holes, so the footprint takes it to start 1.75 mm before the first contact row and run back over the
  pegs (courtyard 0.5 mm bigger all round): check this against a jack in hand.

## Schematic blocks

As on [`ConsoleBoard`](../ConsoleBoard/README.md#schematic-blocks) (MCU, power, USB, CAN, OLED, IR), except:

| Block | Parts | Notes |
| --- | --- | --- |
| Knobs | J4 Select, J5 Volume: RJ11 6P4C top-entry jacks; contacts 2 and 5 to GND, 3 and 4 to ADC1 with a 33k pull-up + 10 nF each | See [Knobs](#knobs-rj11) |
| Keys, LEDs | None | Back / View, the four key LEDs, Q1 and their resistors are gone |

### Pins

| ESP32 GPIO | Signal | ESP32 GPIO | Signal |
| --- | --- | --- | --- |
| 12 / 11 / 10 | OLED SCK / MOSI / CS | 1 / 2 | Select knob, jack contacts 3 / 4 (ADC1) |
| 9 / 8 | OLED D/C / RST | 4 / 7 | Volume knob, jack contacts 3 / 4 (ADC1) |
| 5 / 6 | CAN TX / RX | 39 | IR in |
| 19 / 20 | USB D- / D+ | | |

GPIO 13-18, 21, 38, 47 and 48 are free. Today's console firmware reads the encoders and keys as digital pins, so this
board needs firmware that reads the knobs on the ADC; the firmware can draw the soft-key labels in the screen's bottom
corners and take them from one knob's ring (left / right).

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
python3 generate/gen_footprints.py                             # ConsoleBoardRS.pretty
python3 generate/gen_sch.py                                    # also writes generate/refs.json
JAVA=/opt/homebrew/opt/openjdk@17/bin/java $KIPY generate/gen_pcb.py --route freerouting.jar
python3 generate/drc.py
python3 ../tools/fab_export.py ConsoleBoardRS
```
