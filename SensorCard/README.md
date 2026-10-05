# Sensor card

The dash's **sensor pack on a slot card** for the DigiDash rev 2 board: **80 × 60 mm**, standing on edge in a 2×15
slot, its right-angle header on the bottom edge (`docs/hardware/accessory_ports.md`, rev 2 section). It reads the
senders, the speed sensor, the tach, ignition, battery and the alternator lamp terminal, keeps the odometer in FRAM,
and sends the gauges' frames on CAN. It used to be part of the main board; it moved to a card so that a change to the
sender, speed or tach wiring means a new card, not a new main board.

- an **ATmega2560-16AU** (TQFP-100, 16 MHz, 5 V): the same chip and clock as the rev 2 plan, so
  `arduino/sketches/sensor_pack` ports with pin-map changes (table below)
- an **MCP2515** CAN controller (own 16 MHz crystal) and an **MCP2562** transceiver (VDD / VIO 5 V, STBY low, no
  termination): **fixed CAN node `0x04`** in any slot. It does not read the slot ID
- an **MB85RC256V** FRAM (0x50): the odometer lives on the card. A replacement card starts blank; set its mileage in
  firmware
- the sensor front end, with the changes worked out for rev 2: **sender inputs configured by solder jumpers**
  (pull-up or pull-down, and a bank of bias values per channel), a 5 V / 12 V speed sensor supply, a 10k speed input series resistor, the tach on its own connector with a
  22k optocoupler pull-up, and an ignition divider that stays HIGH through cranking
- the **sensor serial out** (J8): the 36-byte frames on USART1, for an optional jumper wire to the main board's J27,
  which fans them out to the three screens (the fallback path when CAN is down)

**The full specification, configuration guide and sender tables are in [`DATASHEET.md`](DATASHEET.md).** How the
readings are calibrated (stored on the card, set from the center's menu, backed up on the center):
[`docs/protocols/sensor_calibration.md`](../docs/protocols/sensor_calibration.md).

**Any slot but slot 1** (slot 1 is kept for the audio card's tall parts). Power comes from the slot's 5 V only; the
slot's switched 12 V and constant 12 V are used for sensing and the speed sensor's 12 V option.

| Files | |
| --- | --- |
| `SensorCard.kicad_sch` / `.kicad_pcb` / `.kicad_pro` | The card: 80 × 60 mm, 4 layers, all parts on the parts side (F). KiCad 10 |
| `DATASHEET.md` | The card's datasheet: ratings, electrical characteristics, pinouts, jumper configuration, sender selection, firmware interface, mechanical. Images in `doc/` |
| `SensorCard_BOM.csv` | Parts list, every part with maker and part number, plus the PCB solder jumpers (`python3 generate/gen_bom.py`, which also writes `fab/SensorCard_BOM_PCBWay.csv`) |
| `fab/` | Gerbers, drill, placement and centroid (`python3 ../tools/fab_export.py SensorCard`), and the assembler's BOM `SensorCard_BOM_PCBWay.csv` (placed parts only) |
| `generate/` | `gen_sch.py`, `gen_pcb.py`, `drc.py`, `sexp.py` (`refs.json` is written by `gen_sch.py`). The schematic generator is the main board's (`DigiDash2/generate/gen_sch.py`) helpers plus not-fitted parts (none at present); the PCB generator is the GPS card's, adapted. Every symbol and footprint is stock KiCad 10 |

## Connectors

All harness connectors are on the **top edge** (the case panel); the programming and serial headers are right angle
on the **left edge**, reachable with the card in its slot. Pin 1 is the square pad.

| Ref | Silk | What | Pins (1, 2, ...) |
| --- | --- | --- | --- |
| J1 | — | Slot (right-angle 2×15, bottom edge) | Used: 1 / 2 +5 V, 3 / 4 / 7 / 19 / 20 GND, 5 CANH, 6 CANL, 8 switched 12 V, 10 constant 12 V, 12 WAKE. Everything else open (3.3 V, slot ID, LAMP, REV, I2S, audio, AUX 12 V) |
| J2 | FUEL … G | Senders, JST PH 12 pin, side entry | 1 FUEL, 2 FUEL return, 3 ETEMP, 4 ETEMP V (5 V, 0.1 A PTC), 5 ETEMP return, 6 OILP, 7 OILP V (5 V, 0.1 A PTC), 8 OILP return, 9 AMB, 10 AMB return, 11 ALT L, 12 GND |
| J3 | PWR SPD G G | Speed sensor, JST PH 4 pin, side entry | 1 sensor power (5 V or 12 V by JP1, 0.1 A PTC), 2 signal, 3 GND, 4 GND / shield |
| J4 | TACH G | Tach, **Molex Micro-Fit 3.0, 2 pin** (rated well above a coil's kickback, unlike JST PH) | 1 coil negative, HEI tach terminal or an ignition box's tach output, 2 GND |
| J5 | J5 | RPM select, 2×2 vertical | jumper 1-2 = A, 3-4 = B (pulses per revolution, as before) |
| J6 | ISP | AVR ISP, 2×3 right angle | 1 MISO, 2 5V, 3 SCK, 4 MOSI, 5 RST, 6 GND. Power the card from its slot |
| J7 | GND … DTR | FTDI, 1×6 right angle (USART0, 5 V) | 1 GND, 2 CTS (grounded), 3 not connected, 4 RXI (adapter TX), 5 TXO (adapter RX), 6 DTR (auto-reset) |
| J8 | GND TX RX | **Sensor serial out**, 1×3 right angle (USART1, 5 V, 115200 8N1) | 1 GND, 2 TX (frames out, 100 Ω series), 3 RX (in, unused in the car) |
| J9 | J9 | Demo jumper, 1×2 vertical | fitted = fake sweep (bench only); 1 DEMO, 2 +5V |
| J10 | J10 | Spare I/O, 2×5 vertical | 1 +5V, 2 GND, 3 ADC10, 4 ADC11, 5 ADC12, 6 ADC13, 7 D44, 8 D45, 9 D46, 10 D47 |
| JP1 | 5V 12V | Speed sensor supply, solder jumper | 1-2 bridged as delivered = 5 V; cut it and bridge 2-3 = the slot's switched 12 V |
| JP2–JP16 | SENDER CONFIG | Sender topology and bias values, solder jumpers in a labelled block | see [Sender channels](#sender-channels) and the datasheet |

**Serial fallback wiring (optional):** a 2-wire Dupont jumper from **J8 pin 2 (TX)** and **pin 1 (GND)** to the main
board's **J27 pin 1 (TX IN)** and **pin 2 (GND)**. J27 is on the main board's back at the top edge, above slot 4,
within reach of a card's left edge in any slot. Without it the screens take the sensor frames from CAN only.

## Sender channels

Each analog channel has a **topology jumper** (pull-up to +5 V, or pull-down to GND) feeding a **bank of bias
resistors**, each behind its own 2-pad solder jumper; bridge one value (two give their parallel value). The jumpers
sit in the labelled **SENDER CONFIG** block on the parts side (one column per channel: PU / PD at the top, the values
below, `*` = as delivered), and the back silkscreen repeats the table. The pull-ups come straight from +5 V, not
through the channel's PTC, because the PTC's drop would read as a level error. Every channel then goes through 1k with
10 nF into the ADC.

| Channel | Topology | Bank (jumper) | As delivered |
| --- | --- | --- | --- |
| Fuel (ADC0) | JP2 | 100 Ω, 2010 (JP6) · 220 Ω, 1206 (JP7) · 470 Ω, 0805 (JP8) | pull-up, 100 Ω (the factory 1964 0–30 Ω sender) |
| Engine temp (ADC1) | JP3 | 220 Ω, 1206 (JP9) · 1k (JP10) · 10k (JP11) | pull-up, 1k |
| Oil pressure (ADC2) | JP4 | 220 Ω, 1206 (JP12) · 1k (JP13) · 100k (JP14) | pull-up, 220 Ω |
| Ambient (ADC3) | JP5 | 10k (JP15) · 100k (JP16) | pull-up, 100k |

Each bank resistor is sized for its value's worst case (a shorted sender puts the full 5 V across it). Which setting
suits which sender, with the ADC ranges, is in [`DATASHEET.md`](DATASHEET.md#6-configuration).

- **ETEMP V / OILP V** are 5 V through their own 0.1 A PTC, watched by TEMP_MON / PRESS_MON (the fuse monitors). They
  only matter for 2-wire and 3-wire senders.
- **Returns:** pins 2, 5, 8, 10 and 12 are ground returns for each sender. For a 1-wire sender grounded through the
  block or the tank, the body is the reference: keep the engine-to-body and body-to-dash ground straps solid, and run
  the fuel sender's return wire (from its ground terminal or the tank strap) back to pin 2.
- **The firmware's conversion curves change** with the fitted resistors (a pull-up reverses the direction from the old
  pull-down wiring): see [Firmware](#firmware-to-follow).

## Other inputs

| Input | Circuit | Notes |
| --- | --- | --- |
| Speed (ICP5) | J3 pin 2 → 10k pull-up to 5 V, 10k series, BAT54S clamp, 1 nF | Open-collector or push-pull up to 12 V (a 12 V output puts under 1 mA into the clamp). A 2-wire magnetic (VR) pickup needs an interface (LM1815 / MAX9926) that the card does not have |
| Tach (ICP4) | J4 pin 1 → 2 × 4.7k (1206) → PC817 LED (1N4148W reverse clamp) → 22k pull-up, 1k, 1 nF | The coil-side nets (TACH, TACH_R) have their own net class with **1.0 mm clearance** and stay in the top-right corner behind J4 |
| Ignition (PA1) | Slot pin 8 (switched 12 V) → 15k / 10k, 100 nF, BAT54S clamp | HIGH from about 7.5 V, so cranking sag does not drop it |
| Battery (ADC8) | Slot pin 10 (constant 12 V) → 100k / 22k, 100 nF | 16 V → 2.9 V. On port K, not on the JTAG pins (ADC4–7), so no fuse to set |
| Alternator L (PA0) | J2 pin 11 → 22k / 10k, 100 nF, BAT54S clamp | |
| WAKE | PWR_HOLD (PC1) → 4.7k / 10k (3.4 V) → BAT42W → slot pin 12; slot pin 12 → 47k → ADC9 | Holds the main board's 5 V rail after key-off; reads whether another card woke the dash |

## MCU pins (ATmega2560-16AU, TQFP-100)

| Pin | Port | Function | To |
| --- | --- | --- | --- |
| 2 / 3 | PE0 / PE1 | RXD0 / TXD0 | J7 FTDI (USART0) |
| 6 | PE4 | INT4 | MCP2515 INT |
| 19 / 20 / 21 / 22 | PB0 / PB1 / PB2 / PB3 | SS / SCK / MOSI / MISO | MCP2515 CS (10k pull-up), and J6 ISP |
| 26 | PB7 | GPIO (Arduino D13) | status LED (high = on) |
| 30 | RESET | | 10k pull-up, J6, FTDI DTR (100 nF), MCP2515 RESET |
| 33 / 34 | XTAL2 / XTAL1 | | Y1 16 MHz |
| 35 | PL0 | ICP4 | tach |
| 36 | PL1 | ICP5 | speed |
| 37–40 | PL2–PL5 | D47–D44 | J10 spare |
| 43 / 44 | PD0 / PD1 | SCL / SDA | FRAM (4.7k pull-ups) |
| 45 / 46 | PD2 / PD3 | RXD1 / TXD1 | J8 sensor serial (RX 10k pull-up, 1k series; TX 100 Ω series) |
| 53 | PC0 | GPIO | DEMO (10k pull-down; J9 to 5 V) |
| 54 | PC1 | GPIO | PWR_HOLD (WAKE) |
| 71 / 72 | PA7 / PA6 | GPIO, pull-up | RPM select B / A (J5) |
| 74 / 75 | PA4 / PA3 | GPIO | PRESS_MON / TEMP_MON (sender feed fuse monitors) |
| 77 / 78 | PA1 / PA0 | GPIO | IGN_SENSE / ALT_L |
| 84–87 | PK5–PK2 | ADC13–ADC10 | J10 spare (pin-change capable: a flow-sensor pulse, for example) |
| 88 | PK1 | ADC9 / PCINT17 | WAKE sense |
| 89 | PK0 | ADC8 | battery sense |
| 94–97 | PF3–PF0 | ADC3–ADC0 | ambient, oil pressure, engine temp, fuel |
| 98 / 100 | AREF / AVCC | | 100 nF / ferrite from 5 V |

Changes from the main-board pin map in the rev 2 plan: battery sense moved from ADC4 to ADC8 and the spare analog pins
from ADC5–8 to ADC10–13 (ADC4–7 share pins with JTAG, which a factory-fresh 2560 has enabled); the fuel feed monitor
(PA2) and the charge-channel monitor (PA5) are gone (the fuel pull-up no longer runs through a PTC, and the charge
channel never had a supply).

## Board

- **Outline:** 80 × 60 mm, no mounting holes: the slot and the case hold it.
- **Slot header (J1):** placed exactly as on the GPS and audio cards (pin 1 is 17.78 mm right of the centre, seen from
  the parts side, top edge up; the plastic body's face on the bottom edge, the pins 6 mm past it). `gen_pcb.py` checks
  the orientation from the pads.
- **Top edge:** J2 (centre x 27), J3 (x 50), J4 (x 71.5), each with its plug side on the edge. JP1 behind J3, the
  optocoupler U5 behind J4.
- **Left edge:** J8 (pins at y 12–17), J7 (y 22–35), J6 (y 40–45), right angle, the pins 6 mm past the edge, with
  their pin legends beside them.
- **Middle:** the 2560 at (32, 30), its ADC side toward J2, the sender front end between them; the MCP2515 and its
  crystal at the right; the MCP2562 above the slot's CAN pins and the FRAM beside it; J5 / J9 (jumpers) at the
  bottom left, J10 (spare) at the right edge.
- **Parts side (F):** everything; tallest are the vertical pin headers (8.5 mm) and J4 (about 10 mm), under the 20 mm
  limit. **Back (B):** no parts.
- **Layers and rules:** the GPS card's: F signal, In1 solid GND, In2 signal / GND fill, B signal / GND fill. 0.2 mm
  tracks, 0.3 mm on the supplies, 0.13 mm clearance, 0.6 / 0.3 mm vias, plus the **HV** class (TACH, TACH_R: 1.0 mm
  clearance).
- **Sender config block:** the solder jumpers JP2–JP16 in a fixed grid between the 2560 and the MCP2515 (`CFG` at the
  top of `gen_pcb.py`), labelled on the silk; the back silkscreen carries the configuration table. Not-fitted (DNP)
  parts, if any are added, are marked DNP and left out of the placement files by `tools/fab_export.py`.

## Status

**Schematic: done** (114 parts, 16 of them solder jumpers; nothing not-fitted). **PCB: placed and routed**, DRC
(`generate/drc.py`, with the schematic parity check): **no errors, nothing unconnected, parity clean.** The only
warnings are 42 silkscreen-past-the-edge ones (the right-angle headers' and the top-edge connectors' outlines, which
stand past the edges on purpose, as on the GPS card). 997 track segments, 308 vias (163 GND stitching). ERC: no errors. Fab outputs in `fab/` (98 placed parts:
the solder jumpers are copper only). Renders in `doc/`.

How it got there, in case a re-run differs: one Freerouting round (120 passes, with the 0.01 mm `ROUTE_MARGIN`)
routed everything, with no hand edits. The margin exists because an earlier run left one track 0.8 µm inside the
clearance rule. The finisher also tries every pad-to-pad pair through one via (it closed a FUEL link an earlier
placement left open); `--from-ses` repeats the import and finishing steps from the last session.

## Check before ordering

- **The delivered jumper settings** against the senders actually fitted to the car (the datasheet's tables). They
  can be changed on the bench with a blade and a soldering iron; the delivered fuel setting suits the factory 1964 0–30 Ω sender (100 Ω);
  the rest are a guess: 1k temp, 220 Ω oil (10–180 Ω), 100k ambient NTC, all pull-up.
- **Bridged solder jumpers:** the as-delivered ones (JP1, JP2–JP5 1-2, JP6, JP10, JP12, JP16) are bridged by a thin
  copper trace in the footprint, so the card works with no solder work. Confirm the assembler leaves those traces alone.
- **The speed sensor's supply and output** (JP1, and that it is a hall / square-wave sensor, not a 2-wire VR pickup).
- **J4's rating and the tach clearance** against the ignition: points and HEI kick back a few hundred volts on the coil
  negative. The 1206 resistors (two in series) and the 1.0 mm clearance are sized for that; an ignition box's tach
  output is a 12 V square wave and needs nothing more.
- **Crystal load caps:** 18 pF for CL ≈ 10 pF crystals; change them with the crystals you buy.
- **J1 pin 1** against the main board's slot socket in the 3D viewer, and that J2 / J3 / J4 face off the top edge.
- The card outline (80 × 60 mm) and the connector positions against the case panel.

## Firmware

[`firmware/sensor_card`](../firmware/sensor_card/README.md) (PlatformIO, ATmega2560). Written and host-tested, **not yet run on a
card**. It uses the pin map above; because those are an Arduino Mega's pins it also runs on a Mega 2560 with a CAN shield
on the bench (that README says how). What it does of the plan that used to be listed here:

- the pin map above (ICP4 / ICP5 input capture for tach / speed, USART1 for the frames, USART0 for the console and the USB link): done;
- the MCP2515 sending `0x100`–`0x104` at 20 Hz and heartbeat `0x704` as **node `0x04`**: done;
- **ignition from the slot's switched 12 V** (PA1), battery voltage from ADC8 (scale 122 / 22), and PWR_HOLD driving WAKE after key-off until the FRAM work is done: done;
- **calibration** per [`docs/protocols/sensor_calibration.md`](../docs/protocols/sensor_calibration.md): presets, captured points in FRAM, the menu items on the UI channel, the bench console, SENSOR values: done (the old thermistor maths and `*_INVERT_ADC` are gone); not done: frame v5 on the serial link, `cal export` / `cal import`;
- the odometer: a blank FRAM is seeded with `ODO_INITIAL_TENTHS` (`main.cpp`); set the car's mileage there, or with `odo set` on the console, when a card is replaced: done.

## Regenerating

KiCad 10 with its stock libraries, numpy in KiCad's python, and Freerouting **1.9.0** (Java 17 or newer; it opens its
window while it routes):

```bash
KIPY=/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3
python3 generate/gen_sch.py                       # also writes generate/refs.json (part groups, DNP flags)
kicad-cli sch erc SensorCard.kicad_sch            # expect warnings only (library nickname, off-grid)
JAVA=/opt/homebrew/opt/openjdk@17/bin/java $KIPY generate/gen_pcb.py --route freerouting-1.9.0.jar   # place, route, fill
python3 generate/drc.py                           # DRC with the schematic parity check
python3 ../tools/fab_export.py SensorCard         # Gerbers, drill, placement, centroid
```

Temporary files go to `$TMPDIR` with a `sensorcard` prefix. `--stitch` / `--fill` re-run only the ground stitching /
zone fill on the routed board. `--from-ses` rebuilds the pre-route board from the placed copy and repeats the import,
finishing and stitching from the last Freerouting session (a couple of minutes). `FR_TRIES=n` (with a comma-separated
`FR_PASSES` list) re-runs Freerouting from the placed board if a round leaves something unrouted.
