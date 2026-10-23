# Cable harnesses to order

Every cable the dash needs, from the board connectors on the 2026-10-23 panel order. Board connectors are fitted by
the assembler; this list is the **mating side**. Lengths depend on where things go in the car: measure first.

Wire: **18 AWG** for vehicle power and AUX, **20–22 AWG** for the other Micro-Fit lines, **24–26 AWG** for JST PH
(PH crimps take 24–32 AWG, 2 A per pin). Run every CAN pair (H / L) as a **twisted pair**.

## Needed for the car

| # | Board connector | Goes to | Mating housing | Terminals | Notes |
| --- | --- | --- | --- | --- | --- |
| 1 | DigiDash2 **J7** POWER IN, Micro-Fit 3.0, 6 pin | Fuse box (battery), ground, ignition, dash-light feed | Molex **43645-0600** | **43030-0038** (18 AWG) on 1–4; 43030-0007 (20–24 AWG) on 5–6 | 1 12V, 2 12V, 3 GND, 4 GND, 5 IGN, 6 LAMP. About 10 A: two pins each for battery and ground. Fuse the battery feed at the fuse box |
| 2 | DigiDash2 **J13** AUX 12V IN, Mini-Fit Jr, 2 pin | Its own fused feed from the battery / fuse box | Molex **39-01-2020** (5557-02R) | **39-00-0038** (18–24 AWG) × 2 | 1 12V, 2 GND. Sized for 3 A. In-line fuse at the source, plus F1 on the board |
| 3 | DigiDash2 **J17** CABLE 1, JST PH 4 | Console board **J3** (JST PH 4) | JST **PHR-4** both ends | **SPH-002T-P0.5S** × 8 | Straight through, pin 1 to pin 1: 1 5V, 2 CANH, 3 CANL, 4 GND. Twist 2 / 3 |
| 4 | Console **J4 / J5**, RJ9 4P4C | The two RetroSound knobs | — | — | The knobs' own 4P4C leads. For extensions: 4P4C handset cords, either kind (straight or reversed both work, by design) |
| 5 | Sensor card **J2** senders, JST PH 12 (side entry) | Fuel, water temp, oil pressure, ambient, alternator L | JST **PHR-12** | **SPH-002T-P0.5S** × 12 | 1 FUEL, 2 FUEL ret, 3 ETEMP, 4 ETEMP V, 5 ETEMP ret, 6 OILP, 7 OILP V, 8 OILP ret, 9 AMB, 10 AMB ret, 11 ALT L, 12 GND |
| 6 | Sensor card **J3** speed, JST PH 4 (side entry) | Speed sensor | JST **PHR-4** | **SPH-002T-P0.5S** × 4 | 1 sensor power (5 V, or switched 12 V by JP1), 2 signal, 3 GND, 4 GND / shield. Shielded cable |
| 7 | Sensor card **J4** tach, Micro-Fit 3.0, 2 pin | Coil negative / HEI tach terminal | Molex **43645-0200** | **43030-0007** × 2 | 1 tach, 2 GND |
| 8 | GPS card **J2**, JST PH 4 (side entry) | GPS puck on the dash | JST **PHR-4** | **SPH-002T-P0.5S** × 4 | 1 puck power (3.3 V; 5 V by JP1), 2 TX (to the puck's RX), 3 RX (from the puck's TX), 4 GND. The puck's own plug is usually JST GH / SH: cut it off or make an adapter |
| 9 | Radio card **J2**, SMA jack | The car's whip (Motorola plug) | — | — | DIN 41585 female to **SMA male** pigtail (not RP-SMA), e.g. Eightwood TB0188 (10 cm RG174) |
| 10 | Radio card **J5**, 1×2 header 2.54 mm | AM ferrite loopstick | 2-pin 2.54 mm female (Dupont) housing | 2 female Dupont crimps | Optional, for AM. 1 AM node, 2 GND |
| 11 | Audio card **J2**, 2×2 RCA | Amplifier inputs (front L / R, rear L / R) | — | — | Two stereo RCA male-male cables |
| 12 | Audio card **J6**, 3.5 mm | Mic and / or aux in | — | — | 3.5 mm TRS plug: tip mic, ring aux, sleeve GND (pin functions still to be checked on the real jack) |
| 13 | DigiDash2 **J9** CAMERA, JST PH 4 | Backup camera power and reverse trigger | JST **PHR-4** | **SPH-002T-P0.5S** × 4 | 1 REV (reverse lamp 12 V in), 2 12V (switched camera power), 3 unused, 4 GND |
| 14 | DigiDash2 **J11** VIDEO IN, RCA | Backup camera video | — | — | 75 Ω composite RCA male-male, long enough to reach the tailgate |
| 15 | DigiDash2 **F1** | — | — | — | A 5 A mini blade fuse (Littelfuse **0297005.WXNV**), plus spares |

## Only if used

| Board connector | Use | Mating parts |
| --- | --- | --- |
| DigiDash2 **J25** TRUNK, Micro-Fit 3.0, 4 pin | CAN and switched 12 V to the rear of the car | Molex 43645-0400, 43030-0007 × 4. 1 H, 2 L, 3 12V, 4 G. Switch SW1-1 off and terminate the far end |
| DigiDash2 **J22 / J23 / J24** CABLE 2–4, JST PH 4 | More CAN nodes | PHR-4 + SPH-002T-P0.5S, same pinout as J17 |
| DigiDash2 **J6** SERIAL, JST PH 4 | 3.3 V serial device | PHR-4 + SPH-002T-P0.5S. 1 3V3, 2 TX (out), 3 RX (in), 4 GND |
| Sensor card **J8** to DigiDash2 **J27** | Sensor frames over serial instead of CAN (fallback) | Two female-female Dupont jumpers: J8 pin 2 (TX) to J27 pin 1, J8 pin 1 (GND) to J27 pin 2 |

## Bench only

| Connector | Cable |
| --- | --- |
| DigiDash2 **J16** CAN TAP (1×3) | Dupont jumpers to a USB-CAN adapter: 1 H, 2 L, 3 GND |
| GPS / Radio **J3** SWD (1×5) | ST-Link with Dupont leads: 1 3V3 sense, 2 SWDIO, 3 SWCLK, 4 NRST, 5 GND |
| GPS / Radio **J4**, Sensor **J8** debug serial (1×3) | 3.3 V USB-serial with Dupont leads (sensor card: 5 V) |
| Sensor card **J6** ISP (2×3), **J7** FTDI (1×6) | USBasp / AVRISP mkII 6-pin cable; 5 V FTDI cable |
| Audio card **J10** (1×5) | USB-serial for the ESP32 |
| Console **J2** USB-C | USB-C cable |

## Buying tips

- JST PH needs a proper crimper (e.g. Engineer PA-09). Pre-crimped PH leads (SPH terminal on one end) save most of the
  work: buy them, then push them into PHR housings.
- Micro-Fit 3.0 and Mini-Fit Jr: Molex sells pre-crimped leads (Micro-Fit 214760 / 214761 series, 18 AWG), or use a
  Molex 63819-0000 / generic Molex-style crimper.
- Buy spare housings and about 20 % spare terminals of each kind.
