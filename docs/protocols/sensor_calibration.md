# Sensor calibration

How the sensor card (`SensorCard/`, CAN node `0x04`) turns raw ADC counts into fuel, temperature, pressure, voltage and
speed; where that calibration is stored; how it is set from the center screen's menu or a bench serial console; and
how it is backed up. **Status: specification. Nothing here is implemented yet**; it is the plan for the sensor card's
firmware and the matching center / screen changes.

## Why the card owns it

Today the card sends raw counts, and every display converts them with fixed formulas in
`firmware/elc_can/include/elc_sensor_units.h` (fuel and oil as straight lines across 0–1023, coolant as the old 100k
NTC). That cannot follow the senders in a real car, and it is compiled into four boards.

The counts only mean something together with the card's **sender jumpers** (pull-up or pull-down, and the bias
resistor: `SensorCard/DATASHEET.md`, section 6). So the calibration lives **on the card, next to the jumpers**:

- it travels with the card: re-jumpering or replacing the card can never leave a stale calibration on the screens;
- **one conversion**, done once: every screen, the console and the card's own MPG / distance-to-empty use the same
  gallons, degrees and psi;
- the card's **FRAM** has the room (32 KB, of which the odometer uses 64 bytes) and no wear limit, and the odometer's
  two-copy, CRC-checked write scheme is already there.

The **center screen** is where a person sets it (its menu), and it keeps a **backup** on its SD card, so a replacement
card gets the car's calibration back.

```
 senders ─► card: raw counts ─► calibration (FRAM) ─► real units ─► CAN SENSOR 0x124 ─► screens, console, center
                     ▲                                                       │
                     │  UI SET (captures, settings) ◄── center menu ◄────────┘
                     └─ restore ◄── center SD backup (/accessories/0008_04.json)
```

## What is calibrated

| Channel | Input | Output (stored in canonical units) | How |
| --- | --- | --- | --- |
| Fuel | ADC0 | volume, L ×10 (shown as gal or L) and percent of the tank | sender preset + captured points (at fill-ups) |
| Engine temperature | ADC1 | °C ×10 | sender preset, offset, captured points |
| Oil pressure | ADC2 | kPa | captured points (zero with the engine off, then against a gauge), or entered on the bench |
| Ambient temperature | ADC3 | °C ×10 | NTC choice (formula) + offset |
| Battery | ADC8 | V ×100 | trim against a meter |
| Road speed / odometer | ICP5 | pulses per mile | entered, or measured against GPS speed |
| Engine speed | ICP4 | pulses per revolution | J5 jumpers, or a setting that overrides them |

Not calibrated here: the alternator and ignition inputs (on / off), and fuel economy (derived from calibrated fuel
volume and distance).

## The conversion

Per analog channel the card holds:

| Field | Meaning |
| --- | --- |
| topology | pull-up or pull-down: **must match the channel's topology jumper** (the card cannot read the jumpers) |
| bias Ω | the bridged bank value, or the parallel value of several: **must match the value jumpers** |
| preset | a built-in sender curve (in ohms), or none |
| offset | added to the result (temperature channels) |
| points | up to 8 (fuel), 6 (temperature) or 4 (oil) captured or entered points: raw count → value |

**Raw to value:**

1. **Two or more points:** piecewise-linear interpolation between the points, sorted by raw count. Beyond the end
   points the end segment is extended, then clamped to the sensor's display range. The preset is not used.
2. **One point:** the preset curve, shifted so it passes through that point (a one-point correction).
3. **No points:** the preset curve alone. A preset is in sender ohms; the card turns the reading into ohms with the
   topology and bias settings (pull-up: R = R<sub>b</sub> × raw / (1023 − raw); pull-down: R = R<sub>b</sub> × (1023 − raw) / raw).
4. **No points and no preset:** the channel is **not calibrated**: its SENSOR status is 1 (not ready) and the gauge
   shows `--`.

The offset is applied last. Points are stored as raw counts, so they stay right if only the preset or bias setting
is changed; changing the **jumpers** invalidates them (the menu says so).

**Open and short detection.** In pull-up mode a reading at or above 1015 counts is an open circuit (status 2): a
broken sender wire pulls the input to 5 V. A reading at or below 2 counts is a short (status 3) only if the
calibration's lowest point is above 20 counts (a 0–30 Ω fuel sender legitimately reads 0 when empty). Pull-down mode
mirrors this.

### Presets

Presets are tables in the card's firmware. Fuel senders are standard; temperature curves must come from the sender
maker's data and are checked against a captured point before they are trusted.

| Channel | # | Preset | Curve |
| --- | --- | --- | --- |
| Fuel | 0 | none (points only) | |
| | 1 | **GM 0–30 Ω** (to 1964: the factory 1964 El Camino sender) | 0 Ω empty, 30 Ω full, linear in ohms |
| | 2 | GM 0–90 Ω (1965–97) | 0 Ω empty, 90 Ω full |
| | 3 | Ford 73–10 Ω | 73 Ω empty, 10 Ω full |
| | 4 | 240–33 Ω (aftermarket) | 240 Ω empty, 33 Ω full |
| | 5 | VDO 10–180 Ω | 10 Ω empty, 180 Ω full |
| Engine temp | 0 | none (points only) | |
| | 1 | 100 kΩ NTC, B 3950 | Beta formula (the old firmware's maths) |
| | 2 | 10 kΩ NTC, B 3950 | Beta formula |
| | 3 | GM 2-wire EFI coolant sensor | table from GM's published resistance chart (to be entered and checked) |
| Ambient | 0 / 1 | 100 kΩ / 10 kΩ NTC, B 3950 | Beta formula |

A float-arm fuel sender in a shaped tank is not linear, so the fuel preset gets the gauge close and the captured
points make it right. Oil pressure senders vary too much for presets; two captured points (zero and one known
pressure) are enough for most.

### Fuel volume, percent and economy

- **Tank capacity** is a setting (default 20.0 gal, the 1964–67 El Camino tank).
- With a preset only, the curve's 0–100 % maps to 0–capacity. Captured points are in volume, so the table is the
  tank's real shape.
- The card's distance-to-empty and MPG use the calibrated volume and capacity (replacing the fixed `FUEL_TANK_GALLONS`
  and the straight-line `adc / 1023` of today's firmware).
- **Damping:** the reading is averaged over about 20 s (the old firmware's EMA, longer), so slosh does not move the
  gauge; a refuel (a rise of more than 3 % held for 10 s with the engine off or the car stopped) is followed at once.

## Where it is stored

**On the card, in the FRAM (MB85RC256V, I²C 0x50).** Layout (addresses in bytes):

| Address | Size | Content |
| --- | --- | --- |
| `0x0000` | 12 | legacy odometer blob (read once for migration, never written) |
| `0x0010` / `0x0030` | 20 / 20 | odometer, slots A / B (unchanged) |
| `0x0050` | 8 | card identity: `C1 D0`, unique ID u32 (random, made at first boot), reserved u8, CRC-8 |
| `0x0100` / `0x0300` | 512 / 512 | **calibration, copies A / B** |

Each calibration copy: `CA 1B` (magic), format version u8 (1), reserved u8, sequence u32, payload length u16, the
payload, CRC-16/CCITT-FALSE over everything before it. Like the odometer, writes alternate between the copies with a
rising sequence number; at boot the newest copy with a good CRC wins, so a write torn by power loss loses at most the
last change. A save happens when a setting changes (at most once a second), and on key-off before the card releases
the 5 V rail.

**Payload, version 1** (little-endian):

| Field | Type |
| --- | --- |
| flags | u16: bit0 at factory defaults (nothing changed yet) |
| fuel, temp, oil: topology u8, bias Ω ×10 u16, preset u8, offset s16, point count u8, points | per channel |
| point | raw u16, value s16 (fuel L ×10, temperature °C ×10, oil kPa) |
| ambient: NTC choice u8, offset °C ×10 s16 | |
| tank capacity, L ×10 | u16 |
| battery trim, ppm | s32 (±50 000 = ±5 %) |
| speed pulses per mile | u32 ×100 |
| tach pulses per revolution | u8 (0 = from the J5 jumpers) |

Unknown trailing bytes are ignored and missing ones take defaults, so a newer firmware reads an older payload.

**Blank FRAM (a new card):** the defaults below, flag bit0 set. **Defaults** match the card as delivered: fuel
pull-up, 100 Ω, preset 1 (GM 0–30 Ω), tank 20 gal; engine temperature pull-up, 1 kΩ, **no preset (not calibrated)**;
oil pull-up, 220 Ω, **not calibrated**; ambient 100 kΩ NTC; battery trim 0; speed 8000 pulses per mile (a common
cable-drive replacement sensor: check it); tach from J5. Temperature and oil read `--` until calibrated, on purpose:
a plausible but wrong oil pressure or coolant reading is worse than none.

## Setting it: the center's menu

The card speaks the accessory channel protocol (`can_channels.md`) as **node `0x04`, product `0x0008`, node type 3
(sensor node)**. It describes its calibration as **UI items**; the center renders them under
**Menu → Accessories → Sensor card** like any accessory's page. The console's knobs work the same page. The card
follows the center's **PREFS** (gal or L, °F or °C, psi or kPa) for the units it shows; it stores canonical units.

### Items

Kinds and flags as in `can_channels.md` (UI). **S** = a setting (flag bit4: the center backs it up). **A** = advanced
(under Service only). **L** = live while viewed. **C** = asks to confirm.

| Item | Parent | Kind | Label | Notes |
| --- | --- | --- | --- | --- |
| 0 | top | group | Fuel | |
| 1 | top | group | Engine temp | |
| 2 | top | group | Oil pressure | |
| 3 | top | group | Ambient temp | |
| 4 | top | group | Battery | |
| 5 | top | group | Speed & tach | |
| 6 | top | group | Service | |
| 10 | 0 | readout, L | Fuel level | gal / L, 0.1 |
| 11 | 0 | readout, L, A | Fuel raw | counts |
| 12 | 0 | number, S | Tank capacity | 5.0–40.0 gal |
| 13 | 0 | choice, S | Bias resistor | "100 Ω (JP6) / 220 Ω (JP7) / 470 Ω (JP8) / 69 Ω (JP6+7) / 83 Ω (JP6+8) / 150 Ω (JP7+8) / 60 Ω (all)": must match the jumpers |
| 14 | 0 | choice, S | Wiring | "Pull-up (JP2 1-2) / Pull-down (JP2 2-3)" |
| 15 | 0 | choice, S | Sender | the fuel presets |
| 16 | 0 | number, C | Fuel in tank now | SET captures a point at this volume (below) |
| 17 | 0 | text | Table | the points, e.g. "0→0.0 · 48→4.0 · 118→10.0 · 236→20.0 gal" |
| 18 | 0 | action, C | Clear table | |
| 20–27 | 0 | number, S, A | Point 1–8 | packed: raw << 16 \| value (u16 of the s16); −1 = empty. For backup only |
| 30 | 1 | readout, L | Temperature | °F / °C, 0.1 |
| 31 | 1 | readout, L, A | Temp raw | counts |
| 32 | 1 | choice, S | Bias resistor | "220 Ω (JP9) / 1 kΩ (JP10) / 10 kΩ (JP11) / 180 Ω (JP9+10) / 215 Ω (JP9+11) / 909 Ω (JP10+11)" |
| 33 | 1 | choice, S | Wiring | pull-up / pull-down (JP3) |
| 34 | 1 | choice, S | Sender | the temperature presets |
| 35 | 1 | number, S | Offset | ±10.0 ° |
| 36 | 1 | number, C | Coolant is now | SET captures a point at this temperature |
| 37 | 1 | text | Table | |
| 38 | 1 | action, C | Clear table | |
| 40–45 | 1 | number, S, A | Point 1–6 | packed |
| 50 | 2 | readout, L | Oil pressure | psi / kPa |
| 51 | 2 | readout, L, A | Oil raw | counts |
| 52 | 2 | choice, S | Bias resistor | "220 Ω (JP12) / 1 kΩ (JP13) / 100 kΩ (JP14) / 180 Ω (JP12+13) / 990 Ω (JP13+14)" |
| 53 | 2 | choice, S | Wiring | pull-up / pull-down (JP4) |
| 54 | 2 | number, C | Oil pressure is now | SET captures a point (0 with the engine off) |
| 55 | 2 | text | Table | |
| 56 | 2 | action, C | Clear table | |
| 60–63 | 2 | number, S, A | Point 1–4 | packed |
| 70 | 3 | readout, L | Ambient | °F / °C, 0.1 |
| 72 | 3 | choice, S | Sensor | "100 kΩ NTC (JP16) / 10 kΩ NTC (JP15)" |
| 73 | 3 | number, S | Offset | ±10.0 ° |
| 80 | 4 | readout, L | Battery | V, 0.01 |
| 81 | 4 | number, S, A | Trim | ±5.00 % |
| 82 | 4 | number, C | Battery is now | SET (a meter's reading) sets the trim |
| 90 | 5 | number, S | Speed pulses per mile | 500–50 000 |
| 91 | 5 | action, C | Calibrate with GPS | below |
| 92 | 5 | text | Speed status | "8000 p/mi (default)", "GPS: 7984 p/mi, applied", "GPS: no fix" |
| 93 | 5 | choice, S | Tach pulses per rev | "From J5 jumpers / 2 / 3 / 4 / 6 / 8" |
| 100 | 6 | text | Calibration status | "Fuel ✓ · Temp not calibrated · Oil ✓ · Speed default" |
| 101 | 6 | action, C | Reset calibration | back to the defaults (the backup on the center is kept until it is overwritten) |
| 102 | 6 | number, C, A | Set odometer | miles, 0.1: writes the FRAM odometer (a replacement card) |
| 103 | 6 | text, A | Card ID | the unique ID, for the backup |

64 items: exactly the protocol's limit per node. A later item has to replace one (the advanced readouts 11, 31 and 51 are the first to go).

### Captures

A **capture** item (16, 36, 54, 82) is a number the person sets to what the real value is right now ("Fuel in tank
now: 10.0 gal"). On SET the card:

1. averages the channel for **5 s** (100 samples; fuel uses the slosh-damped reading),
2. checks it was steady (fuel ±3 counts, the others ±2) — if not, RESULT 1 and the table text says "Unsteady, try
   again",
3. adds the point (raw, value). A point within 3 counts of an existing one replaces it; a full table replaces the
   nearest point,
4. checks the table is still **monotonic** (raw and value both rising, or one rising as the other falls, all the way
   along). If the new point breaks that, it is not kept: RESULT 1, "Doesn't fit the other points: check the jumpers",
5. saves (FRAM), and answers VALUE and RESULT 0. The menu shows the new table.

RESULT comes after the 5 s average; the menu shows the item as busy until then.

### Procedures

**Fuel** (best done over a few fill-ups, or once with cans):

1. Check the jumpers and set *Bias resistor*, *Wiring* and *Sender* to match (as delivered: 100 Ω, pull-up, GM 0–30 Ω).
2. Near empty: fill a known amount, or note what the pump takes at the next fill-up. With the car level and the
   engine off, set *Fuel in tank now* to the gallons in the tank.
3. Repeat at other levels: 5, 10, 15 gal and full is plenty. More points near the bottom of the tank help the
   distance-to-empty most.
4. *Table* shows the points; *Clear table* starts over.

**Engine temperature:**

1. Set *Sender* if the sender's curve is known (or leave none), and the bias and wiring to match the jumpers.
2. Capture at least two points far apart: cold (the engine at the morning air temperature, which *Ambient* shows) and
   hot (an infrared thermometer on the thermostat housing, or a known-good gauge, at full operating temperature).
3. Or, on the bench, put the sender in a pot of water with a thermometer and capture several temperatures while
   heating it. Ground the sender's body to the card's return pin.
4. *Offset* fine-tunes a preset without capturing.

**Oil pressure:**

1. Engine off, key on: set *Oil pressure is now* to 0.
2. With a mechanical gauge teed in: capture at warm idle and at a higher steady rpm.

**Battery:** with a meter on the battery and the engine off, set *Battery is now* to the meter's reading.

**Speed:** set *Speed pulses per mile* from the sensor's documentation, then refine with GPS: on a straight, level
road, hold 40–70 mph steady and choose *Calibrate with GPS*. The card needs a GPS speed with a 3D fix (the GPS card's
motion message, `gps_card.md`) varying less than 1 % for 20 s; it counts speed pulses over that time, divides by the
distance GPS covered, and applies the result if it is within ±20 % of the current setting (otherwise it shows the
result and asks to confirm). A second run should agree to within 0.5 %.

**Tach:** leave *From J5 jumpers*, or pick the pulses per revolution here (it overrides the jumpers).

## Backup and restore

This is the protocol's **settings backup** (`can_channels.md`, UI → Settings backup), unchanged:

- The center stores every VALUE of an item with flag bit4 in `/accessories/0008_04.json` on its SD card (product
  `0x0008`, node `0x04`), with the card's unique ID.
- **A replacement card** announces with flag bit0 set (factory defaults) and a different unique ID: the center
  restores every setting automatically, one SET at a time. The points are settings too, so the tables come back with
  them. A used card moved in from another car (bit0 clear) is not overwritten; its menu page offers
  **Restore saved settings** or **Keep this board's settings**.
- **The odometer is not part of the backup.** A replacement card's mileage is set with *Set odometer* (or `odo set`
  on the bench console).
- Because the card's ATmega2560 has no MAC address, its unique ID is a random u32 made at first boot (from ADC noise
  and the timer) and kept in the FRAM identity block.
- Restoring points onto a card whose **jumpers differ** gives wrong readings; the restore therefore also brings back
  the bias and wiring settings, and the menu's status line shows "Check jumpers" until the card has seen a reading
  inside its table's range.

## Bench console (J7, USART0, 115200)

The same settings over the FTDI header, for setting up a card before it goes in the car, and for typing a sender's
datasheet curve in directly:

| Command | What |
| --- | --- |
| `cal` | print every channel's settings, points and live reading |
| `cal <ch> wiring pu\|pd` · `cal <ch> bias <ohms>` · `cal <ch> preset <n>` · `cal <ch> offset <value>` | settings (`<ch>` = fuel, temp, oil, amb) |
| `cal <ch> capture <value>` | capture a point now (as the menu does) |
| `cal <ch> point <raw> <value>` · `cal <ch> point <ohms>R <value>` | enter a point by raw count, or by sender ohms (converted with the bias and wiring) |
| `cal <ch> clear` | clear the table |
| `cal fuel tank <gal>` · `cal batt <volts>` · `cal speed ppm <n>` · `cal tach ppr <n\|j5>` | |
| `cal speed mile start` / `stop <miles>` | count speed pulses over a measured distance |
| `cal export` / `cal import <hex>` | the whole calibration as one hex line (CRC-checked), to keep or copy to another card |
| `cal reset` | defaults |
| `odo` · `odo set <miles>` | show / set the odometer |

Values are in US units on the console (gal, °F, psi, mph) unless suffixed (`L`, `C`, `kPa`).

## What the readings look like on the bus

- **SENSOR channel, node `0x04` (`0x124`):** the card publishes its readings with the **same sensor numbers,
  quantities and exponents as the center's core sensors** (`can_channels.md`, Core sensors: 0 road speed … 10 trip
  economy), plus **11 battery voltage** (voltage, exponent −2, 1000 ms). Status follows the rules above (1 = not
  calibrated or not ready, 2 / 3 open / short, 5 a fuse-monitor fault).
- **The center** relays them on its own core sensors (`0x121`) while it hears `0x124`, so every existing consumer
  (the console) is unchanged; with no `0x124` it converts the legacy frames as today.
- **The screens** take the gauge values from `0x124` while it is heard within 1 s, and fall back to the legacy raw
  frames and the fixed formulas in `elc_sensor_units.h` otherwise (a rev 1 sensor pack, or a card without this
  firmware).
- **The serial fallback** (J8 → main board J27) carries raw counts today, so a screen running on it alone would show
  uncalibrated values. The sensor UART frame therefore gains the calibrated values (**frame v5**, an extension of
  `sensor_uart.md`): fuel percent ×10 u16, coolant °C ×10 s16, oil kPa u16, battery V ×100 u16, and a calibration
  status byte (bit per channel). That is a wire-format change: **the card and all three screens are flashed together**
  (as the README already requires for any frame change).
- **ALERT:** at key-on, while engine temperature or oil pressure is not calibrated, the card raises one info-level
  alert ("Calibrate the temp / oil sender: Menu → Accessories → Sensor card"). It clears when both are calibrated.

## Work to do

| Where | What |
| --- | --- |
| Sensor card firmware | Port to the card (pin map in `SensorCard/DATASHEET.md`), then: FRAM identity and calibration blocks, the conversion, captures, the channel protocol (NET ANNOUNCE, SENSOR, UI, ALERT), the bench console commands, frame v5 |
| Center firmware | The generic accessory UI renderer and settings backup (already planned in `can_channels.md`), relaying `0x124` into `0x121` |
| Screens and console | Gauges from SENSOR values with the legacy fallback; frame v5 parsing |
| Docs | `can_channels.md`: product `0x0008` (done with this spec); `sensor_uart.md`: frame v5 when it is implemented |
