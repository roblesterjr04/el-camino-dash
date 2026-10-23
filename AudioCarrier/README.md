# Audio card

A **slot card** for the DigiDash rev 2 board: **80 × 60 mm**, standing on edge in a 2×15 slot, its right-angle header
on the bottom edge and its jacks on the top edge (`docs/hardware/accessory_ports.md`, rev 2 section). It goes in
**slot 1**: the RCA block is 28.5 mm tall, and slot 1 is the one whose parts side faces off the end of the row. It is
the clock master of the slot's **I2S rail**, which reaches every other slot (an AI voice card can go in any of them). The directory keeps its old name. On it:
- an **ESP32-WROVER-E-N8R8** module (classic ESP32: A2DP + AVRCP + HFP; 8 MB flash, 8 MB PSRAM, PCB antenna)
- two **TI PCM5102A** stereo DACs (front pair, rear pair)
- a **TI PCM1808** ADC with an electret microphone preamp (hands-free calls)
- a **2 × 2 right-angle RCA block** (Switchcraft PJRAS2X2S01X) and, beside it on the same top edge, a small
  **3.5 mm jack** (Same Sky SJ-3523-SMT) for a hands-free car microphone (no line / aux input)
- a **74LVC125** that puts the I2S clocks, the mic data and MCLK on the slot's link lines, for the partner card

CAN node from its slot (`0x14 + (slot − 1)`: `0x14` in slot 1), 5 V from the slot only. It fits rev 2 only (the 2×5 rev 1 version is in git history).

| Files | |
| --- | --- |
| `AudioCarrier.kicad_sch` / `.kicad_pcb` / `.kicad_pro` | The card: 80 × 60 mm, 4 layers, parts on both sides. Placed and routed (see [Status](#status)). KiCad 10 |
| `AudioCarrier.pretty` | The ESP32-WROVER-E footprint (land pattern from the Espressif datasheet v2.4, figure 15; KiCad has none) and the RCA block (hole pattern from Switchcraft's PJRAS2X2S__X customer drawing), and a plain 3D model of the block. Everything else is stock KiCad (the jack: `Connector_Audio:Jack_3.5mm_CUI_SJ-3523-SMT_Horizontal`) |
| `generate/` | Scripts that produced the files (`gen_footprints.py`, `gen_sch.py`, `gen_pcb.py`, `drc.py`) |

## Signal paths

```
ESP32 I2S0 (master) BCK GPIO14, WS GPIO25, DOUT GPIO27 ──► PCM5102A front ──► 470 R + 2.2 nF ──► J2 FL, J3 FR
ESP32 I2S1 (slave, same BCK / WS)  DOUT GPIO26            ──► PCM5102A rear  ──► 470 R + 2.2 nF ──► J4 RL, J5 RR
+5VA ─ 1k ─ MICV (47u) ─ 2.2k ─┐
J6 tip, electret mic (100p) ────┴ 1u ─ OPA2376 A (gain 22.4) ─ 1u ─► PCM1808 VINL ──► ESP32 I2S0 DIN GPIO35
J6 ring + sleeve = GND;  PCM1808 VINR ─ 1u ─ GND;  OPA2376 B = follower on VMID (spare)
ESP32 GPIO0 (CLK_OUT) 12.288 MHz = 256 fS master clock ──► PCM1808 SCKI      GPIO4 = DAC mute (XSMT, pulled low)

I2S rail (bused to all six slots), through U8 74LVC125 (enabled by GPIO21 low) and 33 R each. The card's net names
are LINK0-4; on the main board they are the rail's I2S_* nets:
  BCK ──► LINK0 = I2S_BCK    WS ──► LINK1 = I2S_WS    PCM1808 DOUT (mic) ──► LINK2 = I2S_MIC    MCLK ──► LINK4 = I2S_MCLK
  LINK3 = I2S_RET (the granted card's audio) ── 33 R ──► GPIO34 = I2S1 data in (100k pull-down)
Audio bus: FL_LINE / FR_LINE ── 100 R ──► AUDIO_L / AUDIO_R;  AUDIO_GND ── 10 R ── GND
Slot ID (pin 11): 10k to 3.3 V here, the slot's resistor to GND on the main board ──► GPIO36 (ADC1)
```

ESP32-WROVER-E pins (datasheet v2.4, table 3). The GPIOs are the ones the PICO-MINI version used, so the firmware's
pin defines are unchanged:

| Pin | GPIO | Net | Pin | GPIO | Net |
| --- | --- | --- | --- | --- | --- |
| 1, 15, 38, 39 (EPAD) | | GND | 25 | IO0 | BOOT strap (10k up, SW2, J10) and MCLK out (CLK_OUT1) → 33 R → PCM1808 SCKI, LINK4 |
| 2 | 3V3 | +3V3 (10 µF + 100 nF) | 26 | IO4 | DAC_MUTE (XSMT, 100k down) |
| 3 | EN | 10k up, 1 µF, SW1, J10 | 33 | IO21 | LINK_EN_N (74LVC125 OEs, 10k up) |
| 4 | I36 (SENSOR_VP) | SLOT_ID (ADC1) | 34 / 35 | RXD0 / TXD0 | J10 |
| 6 | I34 | LINK_SDIN (I2S1 data in, RET) | 16 / 36 | IO13 / IO22 | NODE0 / NODE1 (JP1 / JP2, unused on rev 2) |
| 7 | I35 | ADC_DOUT (I2S0 data in) | 14 | IO12 (MTDI) | open: must read low at reset (3.3 V flash); internal pull-down |
| 8 / 9 | IO32 / IO33 | CAN TXD / RXD (1k) | 24, 23, 29 | IO2, IO15, IO5 | open (boot straps, left at their defaults) |
| 10 | IO25 | I2S WS (both DACs, ADC, LINK1) | 5, 30, 31, 37 | I39, IO18, IO19, IO23 | open |
| 11 | IO26 | I2S1 DOUT → rear DAC | 17–22, 27, 28, 32 | (flash, PSRAM, NC) | not connected |
| 12 / 13 | IO27 / IO14 | I2S0 DOUT → front DAC / BCK (all, LINK0) | | | |

| Jack (J2, the block) | Signal | Insert colour |
| --- | --- | --- |
| Left column, top | Front left (U5 OUTL) | white |
| Left column, bottom | Front right (U5 OUTR) | red |
| Right column, top | Rear left (U6 OUTL) | white |
| Right column, bottom | Rear right (U6 OUTR) | red |
| J6, 3.5 mm jack (Same Sky SJ-3523-SMT) | tip = mic (pin 2); ring (pin 3) and sleeve (pin 1) = GND, so a mono TS plug and a TRS plug both work | top edge, right of the block |

"Left" and "right" are seen from the plug side, standing outside the right edge. The sleeves and the front plate's two
solder tabs are ground.

- **DACs:** 3-wire I2S, SCK tied to ground so the internal PLL runs from BCK (BCK must be 64 fS, which is what the ESP32
  produces with 32-bit slots). FMT, FLT and DEMP low. Outputs are ground-centred 2.1 Vrms: no DC-block caps, just the
  datasheet's 470 R / 2.2 nF filter. The two DACs share BCK and LRCK and have their own data lines, so front and rear are
  independent (fader in software). Both stay muted until the firmware raises GPIO4. Decoupling as in the datasheet
  (LDOO 0.1 µF).
- **ADC:** slave mode (MD0 = MD1 = low), I2S 24-bit, 5 V analog / 3.3 V digital, full scale 3 Vp-p. It runs from the same
  BCK / LRCK as the DACs, so ESP32 I2S0 does full duplex (play + record) in master mode.
- **Mic input (calls only):** for an electret car hands-free microphone on a mono 3.5 mm TS plug ("plug-in power").
  Bias: 2.2 kΩ to the tip from MICV, which is +5VA through 1 kΩ + 47 µF (about 3 Hz; about 4.5 V at a 0.5 mA
  capsule). 100 pF from the tip to ground at the jack (RF). DC-blocked (1 µF into 100 kΩ to VMID = 2.5 V), then
  OPA2376 A non-inverting, gain 1 + 47k / 2.2k = 22.4 (27 dB; 2.2 kΩ + 10 µF to ground, so DC gain 1; 100 pF across
  the 47k, about 34 kHz). A -42 dBV/Pa capsule gives 0.18 Vrms at 94 dB SPL; the PCM1808's ~1 Vrms full scale is
  reached at about 109 dB SPL, so a loud voice close to the mic still has headroom. Out through 1 µF to VINL (the
  left channel, which the firmware reads). VINR is AC-grounded through 1 µF; OPA2376 B is a follower on VMID, its
  output unloaded (no floating inputs). There is no line / aux input.
- **Power:** +5 V from the port; **AP7361C-33E** (Diodes, SOT-223: 1 IN, 2 GND / tab, 3 OUT; 1 A, 0.36 V dropout) for
  the ESP32 and digital sides, 22 µF + 10 µF ceramic in, 22 µF ceramic + 100 nF out, 10 µF + 100 nF at the module.
  The tab sits on a solid ground copper area with 12 vias to the planes. Load: the WROVER-E peaks at about 500 mA
  with RF bursts, the digital sides of the converters, the buffer and the CAN VIO add some tens of mA: (5 − 3.3) V ×
  0.6 A ≈ 1 W peak, a few hundred mW on average. Ferrites make +5VA (ADC analog, op-amp, mic bias) and +3V3A (DAC
  analog and charge pump).

## Board

- **Outline:** 80 × 60 mm, no mounting holes: the slot and the case hold it. The RCA block's own screw hole (in the
  middle of the four jacks) fastens it to the case panel, so plug forces go to the case and not the slot.
- **Slot header (J1):** right-angle 2×15 (`PinHeader_2x15_P2.54mm_Horizontal`) on the parts side, centred on the bottom
  edge: pin 1 is 17.78 mm right of the centre (seen from the parts side, top edge up), the pins count to the left, the
  plastic body's outer face is on the bottom edge and the pins stand 6 mm past it. `gen_pcb.py` checks the orientation
  from the pads, so each pin meets the same-numbered pad of the main board's slot socket.
- **Top edge** (the case panel), from the left: the module's antenna end at the corner (x 1.2–19.2), the RCA block
  (centred at x 52) and the 3.5 mm jack (x 72.3). These positions are constants at the top of `gen_pcb.py`; move them to suit the case.
- **RCA block (J2):** the jacks face off the top edge (right-angle), and the plate's rear face is the board edge. The
  body is 27 mm wide and 13.7 mm deep behind the edge, and **28.5 mm tall** above the card. The barrels (8.2 mm) and
  the 1.5 mm plate stand out past the edge. From the drawing: tip pins 12.5 mm behind the plate, sleeve pins 5.25 mm,
  plate tabs 8.75 mm.
- **Mic jack (J6):** Same Sky SJ-3523-SMT (KiCad's stock footprint; pin functions from the Same Sky datasheet: 1
  sleeve, 2 tip, 3 ring), right of the block on the same edge. Its body (6 wide, 5 tall, 14.5 deep) stands behind the
  edge, its 2.5 mm nose (Ø 5) past it: inside the housing envelope (card x 67.4–76.7, ≈6 mm off the parts face, nose
  to 2.5 mm past the edge). It is SMT (fab-assembled) with two plastic locating pegs. It replaces the Tinkersphere
  TS3328, which no distributor stocks and whose pins were never confirmed.
- **Parts side (F):** the module (3.3 mm tall), regulator, CAN transceiver, link buffer, buttons (below the module), J10,
  the RCA block and the jack.
  In slot 1 this side faces off the end of the row, so its height is not limited.
- **Back (B):** both DACs, the ADC, the op-amp and their passives, all under 1.2 mm tall (the slot spec allows 2.5 mm:
  this side faces the card in slot 2).
- **Layers:** F signal, In1 solid GND, In2 signal / GND fill, B signal. 0.2 mm tracks, 0.3 mm on the supplies,
  0.13 mm clearance, 0.6 / 0.3 mm vias: standard 4-layer rules at JLCPCB / PCBWay.
- **Antenna:** the WROVER-E's antenna end (its 6.22 mm antenna area) is on the top edge at the left corner, flush
  with the edge (0.2 mm in), as Espressif's placement guide prefers when the antenna cannot overhang. Keep-outs: the
  module footprint's (no copper, any layer, over the antenna area) and a board one (no copper and no parts, any layer
  or side) from the left edge to 5 mm right of the module, 7 mm down from the top edge. The module's body is copper
  (pours) underneath, as Espressif allows. Keep metal away from that corner of the case. The module now spans card x
  1.2–19.2 and y 0.2–31.6 (housing: y ≈ 74–92, z ≈ 37–68.3, 3.3 mm off the parts face), bigger than the PICO's
  13.2 × 16.6 mm, which `docs/hardware/housing_envelope.md` still lists (78–93, z 51–68.5).

## Status

**Schematic: done** (the slot card, 90 parts). **PCB: placed and routed** (2026-10-23, rev G): 80 × 60 mm, 4 layers, 90
placed parts (88 in the placement file: JP1 / JP2 are copper only). DRC with the schematic parity check: **no errors,
nothing unconnected, no footprint / parity errors**; 31 silkscreen warnings (text at the edge, over pads, overlapping).
Freerouting (60 passes) left one link of EN (SW1's right-hand pad to C10 / R2); it was joined by hand with a short
B.Cu jumper through two vias (beside SW1 and above R2). A fresh `gen_pcb.py --route` run will not reproduce that and
may leave other links. 580 vias, none touching a pad; nothing in the antenna keep-out. Fab outputs in `fab/`
(`python3 ../tools/fab_export.py AudioCarrier`); `fab/AudioCarrier_BOM_PCBWay.csv` is kept by hand and matches the
placement file; `AudioCarrier_BOM.csv` is the same list.

- **Rev G (2026-10-23), before the PCBWay order:**
  - **Module:** ESP32-PICO-MINI-02-N8R2 (NRND) → **ESP32-WROVER-E-N8R8**. Footprint drawn from the datasheet v2.4
    (figure 15: 38 pads 1.5 × 0.9 on 1.27 mm, centres 17.5 mm apart, pin 1 7.44 mm from the antenna end, pin 19 1.1 mm
    from the far end; EPAD 3.7 mm square, 7.5 mm from the left edge and 16.16 mm from the far end, nine 0.9 mm paste
    openings, no vias in it). Pins checked against table 3. Same GPIOs as before (pin map above).
  - **Regulator:** AMS1117-3.3 + 22 µF tantalum → **AP7361C-33E-13** with ceramics (pinout 1 IN, 2 GND, 3 OUT, from
    the Diodes datasheet); tab solid to a ground copper area with 12 vias.
  - **Mic:** aux path removed (ring buffer, its coupling cap and bias resistor, the RING net); bias now from +5VA
    through 1k + 47 µF; ring and sleeve to ground; OPA2376 B a follower on VMID; PCM1808 VINR AC-grounded.
    Tinkersphere TS3328 → **Same Sky SJ-3523-SMT** (stock footprint, datasheet pinout). The SJ1-3523N was checked
    first: it is 12 mm wide, more than the 9.3 mm the housing allows there.
  - **Moved:** the module (now x 1.2–19.2, y 0.2–31.6), SW1 / SW2 (below the module, y 36), the jack (x 72.3); every
    passive re-placed by the script; re-routed. Fixed: outline, J1, J2, J10, the jack's edge position.
- **The RCA block's footprint** is drawn from Switchcraft's customer drawing
  (`https://switchcraft.com/assets/1/24/pjras2x2s__x_series_cd.pdf`; it is proprietary, so it is not copied here). One
  thing the drawing does not say: which line is the PCB edge. I took the dashed line (the plate's rear face) as the
  edge. If the plate should sit flush with the case panel instead, the board edge moves 1.5 mm (the plate thickness).
  Check with the block in hand and against the case panel cut-out.
- **Earlier checks (2026-10-05), still valid:** PCM1808 (TI SLES177B pin table; MD0 / MD1 low = slave, FMT low = I2S;
  VREF 100n + 10u); PCM5102A (TI pin table; SCK low = internal PLL, FMT low = I2S, LDOO 100n as TI asks); MCP2562;
  OPA2376 (standard dual pinout); 74LVC125 (1 / 4 / 10 / 13 OE, BCK / WS / ADC data / MCLK onto LINK0 / 1 / 2 / 4); J1
  against the slot pinout and DigiDash2's J31–J36; the RCA block against Switchcraft's PC board layout.
- **BOM complete:** every line of `fab/AudioCarrier_BOM_PCBWay.csv` has a maker and part number.
- **Still open:**
  - `docs/hardware/housing_envelope.md` still lists the PICO module (card top corner, y 78–93, z 51–68.5, 2.6 mm);
    the WROVER-E needs y ≈ 74–92, z ≈ 37–68.3, 3.3 mm off the parts face (slot 1: no height limit). Update it and
    keep metal off that larger corner of the case.
  - The jack's panel opening: the SJ-3523-SMT nose is Ø 5 mm, 2.5 mm past the edge, centred 0.3 mm further right than
    the TS3328's (Ø 6) was.
  - The RCA plate edge line (above) and the card's 80 × 60 mm outline against the case.
  - Bench: the electret path (bias, gain, levels with a real car mic) and the regulator's temperature during RF bursts.

## Things to know

- **Firmware:** the audio bridge (`arduino/sketches/esp32_audio_bridge`, PlatformIO env `audiocarrier` in
  `firmware/esp32_classic_bt_audio`). It runs the I2S all the time at 48 kHz with 32-bit slots (`audio_engine.cpp`):
  I2S0 master with MCLK on GPIO0 (12.288 MHz, from the APLL), the front DAC on GPIO27 and the ADC on GPIO35
  (the microphone, left channel); I2S1 a slave on the same BCK / WS through the GPIO matrix, the rear DAC on GPIO26 and
  the rail's RET in on GPIO34. Front and rear start on the same frame and are written in step. GPIO4 un-mutes the
  DACs and GPIO21 turns the rail buffer on once the clocks run (driven high before it becomes an output, so the
  buffer never glitches on at boot). The music (Bluetooth resampled from the phone's rate,
  or the radio from RET) goes through the 10-band equaliser and the master volume; calls, a voice card on RET
  and the chimes are mixed over it, then fader and balance. As the AUDIO_CMD sink (`sink.cpp`) it picks the source,
  hands out RET over NET RAIL (`docs/protocols/can_channels.md`, one holder at a time; a voice card ducks the music),
  and has a page with chime volume, fader, balance and the Equalizer. The serial console's `audio` prints the engine
  (CPU per 5 ms block, the Bluetooth ring and its drift trim, RET and mic levels) and the sink.
- **Not yet tried on a built card:** I2S1 as a slave on I2S0's clock pins (the driver makes them inputs; the firmware
  routes I2S0's BCK / WS back out on the same pins), RET from a radio card, and the electret mic path. Check those
  first on the bench: `audio` shows RET's and the mic's peak levels.
- **Slot:** the slot ID on GPIO36 (ADC1) at boot goes in the NET ANNOUNCE flags and the RAIL answers.
- **Node number:** from the slot ID (GPIO36), `0x14 + (slot − 1)`. The node straps JP1 / JP2 (GPIO13, GPIO22) are left
  over from the rev 1 carrier: the firmware ignores them when it reads a valid slot, and the next revision of the card
  can drop them.
- **GPIO0 is the master clock and the BOOT strap.** It must be high (or free) at reset. The PCM1808 SCKI input is 5 V
  tolerant and just sits on it; the 33 R series resistor damps the clock. Pressing BOOT still enters the loader.
- The module comes from assembly blank: J10 is the only way to load the first firmware. In slot 1 the parts side
  faces off the end of the row, so J10 (vertical) stays reachable with the card in its slot.
- Program it over the 5-pin header J10 (GND, TX0, RX0, IO0, EN, 3.3 V logic) with the BOOT (SW2) and RESET (SW1)
  buttons. Power it from the port, not the adapter.
- Order the module as **ESP32-WROVER-E-N8R8** (8 MB flash, 8 MB PSRAM, PCB antenna; in mass production per the
  datasheet v2.4). The ESP32-PICO-MINI-02 it replaces is NRND and the WROVER-E -R2 variants are EOL. It is the module
  of the bench DevKitC-VE, so the firmware's `esp32wrover` settings (8 MB flash, `BOARD_HAS_PSRAM`) carry over. The
  PSRAM is needed: the bridge keeps its album-art buffers there; a PSRAM-less part would leave Bluetooth ~32 KB of RAM
  while streaming. Its flash uses GPIO6–11 and its PSRAM GPIO16 / 17 (pins 17–22, 27, 28: not brought out); none are
  used here.
- History (in git): the 50 × 52 mm double-bay version, the 50 × 26 mm single-position version (jacks on a JST PH pigtail), the 104 × 70 mm WROOM-32E
  version, the MAX5134 version and the DevKit + MAX98357A version.

## Regenerating

KiCad 10 with its stock libraries, numpy in KiCad's python (`$KIPY -m pip install --user numpy`), and Freerouting
**1.9.0** (Java 17 or newer; it opens its window while it routes). Freerouting 2.1 does not finish this board, and 1.9
hangs if the DSN already has wiring, so the script routes once from scratch and then finishes with its own steps
(joining any leftover piece of a net, ground vias and stitching).

```bash
KIPY=/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3
python3 generate/gen_footprints.py && python3 generate/gen_sch.py   # also writes generate/refs.json (part groups for placement)
JAVA=/opt/homebrew/opt/openjdk@17/bin/java $KIPY generate/gen_pcb.py --route freerouting-1.9.0.jar   # place, route, fill
python3 generate/drc.py
```

`FR_PASSES` sets Freerouting's pass limit (default 120; this board routed completely at 60, so try a few values if a run leaves links behind). `--stitch` / `--fill`
re-run only the ground stitching / zone fill on the routed board. After any schematic change run the generators again
(or, in KiCad, **Update PCB from Schematic** and re-route by hand).
