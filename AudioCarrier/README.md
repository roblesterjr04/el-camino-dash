# Audio card

A **slot card** for the DigiDash rev 2 board: **80 × 60 mm**, standing on edge in a 2×15 slot, its right-angle header
on the bottom edge and its jacks on the top edge (`docs/hardware/accessory_ports.md`, rev 2 section). It goes in
**slot 1**: the RCA block is 26.5 mm tall, and slot 1 is the one whose parts side faces off the end of the row. It is
the clock master of the slot's **I2S rail**, which reaches every other slot (an AI voice card can go in any of them). The directory keeps its old name. On it:
- an **ESP32-DevKitC-VE** (the bench board: ESP32-WROVER-E, classic ESP32 for A2DP + AVRCP + HFP; 8 MB flash, 8 MB
  PSRAM, PCB antenna) plugged into two 1 × 19 female sockets. It brings its own USB programming and 3.3 V regulator,
  so the card has no module, regulator, buttons or programming header to assemble (rev J)
- two **TI PCM5102A** stereo DACs (front pair, rear pair)
- a **TI PCM1808** ADC with an electret microphone preamp (hands-free calls)
- a **2 × 2 right-angle RCA block** (Kycon KLP42X-61-WW/RR) and, beside it on the same top edge, a small
  **3.5 mm jack** (Same Sky SJ1-3533NG, through-hole) for a hands-free car microphone (no line / aux input)
- a **74LVC125** that puts the I2S clocks, the mic data and MCLK on the slot's link lines, for the partner card

CAN node from its slot (`0x14 + (slot − 1)`: `0x14` in slot 1), 5 V from the slot only. It fits rev 2 only (the 2×5 rev 1 version is in git history).

| Files | |
| --- | --- |
| `AudioCarrier.kicad_sch` / `.kicad_pcb` / `.kicad_pro` | The card: 80 × 60 mm, 2 layers, every part on the top. Placed and routed (see [Status](#status)). KiCad 10 |
| `AudioCarrier.pretty` | The DevKitC socket footprint (pin positions and numbers as Espressif's own `Espressif.pretty/ESP32-DevKitC`, rows 25.4 mm apart) and the RCA block (hole pattern from Kycon's KLP42X-61-xx/xx drawing), and a plain 3D model of the block. Everything else is stock KiCad (the jack: `Connector_Audio:Jack_3.5mm_CUI_SJ1-3533NG_Horizontal`) |
| `generate/` | Scripts that produced the files (`gen_footprints.py`, `gen_sch.py`, `gen_pcb.py`, `drc.py`) |

## Signal paths

```
ESP32 I2S0 (master) BCK GPIO14, WS GPIO25, DOUT GPIO27 ──► PCM5102A front ──► 470 R + 2.2 nF ──► J2 FL, J3 FR
ESP32 I2S1 (slave, same BCK / WS)  DOUT GPIO26            ──► PCM5102A rear  ──► 470 R + 2.2 nF ──► J4 RL, J5 RR
+5VA ─ 1k ─ MICV (47u) ─ 2.2k ─┐
J6 tip, electret mic (100p) ────┴ 1u ─ TLV9062 A (gain 22.4) ─ 1u ─► PCM1808 VINL ──► ESP32 I2S0 DIN GPIO35
J6 ring + sleeve = GND;  PCM1808 VINR ─ 1u ─ GND;  TLV9062 B = follower on VMID (spare)
ESP32 GPIO0 (CLK_OUT) 12.288 MHz = 256 fS master clock ──► 33 R ──► PCM1808 SCKI   GPIO4 = DAC mute (XSMT, pulled low)

I2S rail (bused to all six slots), through U8 74LVC125 (enabled by GPIO21 low) and 33 R each. The card's net names
are LINK0-4; on the main board they are the rail's I2S_* nets:
  BCK ──► LINK0 = I2S_BCK    WS ──► LINK1 = I2S_WS    PCM1808 DOUT (mic) ──► LINK2 = I2S_MIC    MCLK ──► LINK4 = I2S_MCLK
  LINK3 = I2S_RET (the granted card's audio) ── 33 R ──► GPIO34 = I2S1 data in (100k pull-down)
Audio bus: FL_LINE / FR_LINE ── 100 R ──► AUDIO_L / AUDIO_R;  AUDIO_GND ── 10 R ── GND
Slot ID (pin 11): 10k to 3.3 V here, the slot's resistor to GND on the main board ──► GPIO36 (ADC1)
```

DevKitC-VE socket pins (U1; numbers as Espressif's KiCad symbol, `ESP32-DevKitC`). The GPIOs are the ones the
module version used, so the firmware's pin defines are unchanged:

| Pin | DevKitC | Net | Pin | DevKitC | Net |
| --- | --- | --- | --- | --- | --- |
| 1 | 3V3 | +3V3 (out of the DevKitC's regulator) | 19 | 5V | +5V from the slot |
| 14, 32, 38 | GND | GND | 25 | IO0 | MCLK (CLK_OUT1) → 33 R → PCM1808 SCKI, LINK4 (also the DevKitC BOOT strap) |
| 3 | VP (IO36) | SLOT_ID (ADC1) | 26 | IO4 | DAC_MUTE (XSMT, 100k down) |
| 5 | IO34 | LINK_SDIN (I2S1 data in, RET) | 33 | IO21 | LINK_EN_N (74LVC125 OEs, 10k up) |
| 6 | IO35 | ADC_DOUT (I2S0 data in) | 7 / 8 | IO32 / IO33 | CAN TXD / RXD (1k) |
| 9 | IO25 | I2S WS (both DACs, ADC, LINK1) | 10 | IO26 | I2S1 DOUT → rear DAC |
| 11 / 12 | IO27 / IO14 | I2S0 DOUT → front DAC / BCK (all, LINK0) | others | | open (EN, VN, 12, 13, 2, 5, 15, 18, 19, 22, 23, TX / RX; 6-11 flash, 16 / 17 PSRAM) |

| Jack (J2, the block) | Signal | Insert colour |
| --- | --- | --- |
| Left column, top | Front left (U5 OUTL) | white |
| Left column, bottom | Front right (U5 OUTR) | red |
| Right column, top | Rear left (U6 OUTL) | white |
| Right column, bottom | Rear right (U6 OUTR) | red |
| J6, 3.5 mm jack (Same Sky SJ1-3533NG) | tip = mic (pin T); ring (R) and sleeve (S) = GND, so a mono TS plug and a TRS plug both work | top edge, right of the block |

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
  TLV9062 A non-inverting, gain 1 + 47k / 2.2k = 22.4 (27 dB; 2.2 kΩ + 10 µF to ground, so DC gain 1; 100 pF across
  the 47k, about 34 kHz). A -42 dBV/Pa capsule gives 0.18 Vrms at 94 dB SPL; the PCM1808's ~1 Vrms full scale is
  reached at about 109 dB SPL, so a loud voice close to the mic still has headroom. Out through 1 µF to VINL (the
  left channel, which the firmware reads). VINR is AC-grounded through 1 µF; TLV9062 B is a follower on VMID, its
  output unloaded (no floating inputs). There is no line / aux input.
- **Power:** +5 V from the port to the DevKitC's 5V pin (10 µF at the header). The DevKitC's own 3.3 V regulator
  runs the ESP32 and, from its 3V3 pin (100 nF there), the card's 3.3 V loads: the converters' digital sides, the
  buffer and the CAN VIO, some tens of mA. Ferrites make +5VA (ADC analog, op-amp, mic bias) and +3V3A (DAC analog
  and charge pump). **Do not plug the DevKitC's USB in while the card is powered from the slot**: both feed its 5V
  pin (check the DevKitC's schematic for a diode before relying on it). To program it in place, unpower the card.

## Board

- **Outline:** 80 × 60 mm, no mounting holes: the slot and the case hold it. The RCA block's plate has a hole in the
  middle of the four jacks; if it takes a screw to the case panel, plug forces go to the case and not the slot (check
  on the part: Kycon's drawing does not say).
- **Slot header (J1):** right-angle 2×15 (`PinHeader_2x15_P2.54mm_Horizontal`) on the parts side, centred on the bottom
  edge: pin 1 is 17.78 mm right of the centre (seen from the parts side, top edge up), the pins count to the left, the
  plastic body's outer face is on the bottom edge and the pins stand 6 mm past it. `gen_pcb.py` checks the orientation
  from the pads, so each pin meets the same-numbered pad of the main board's slot socket.
- **Top edge** (the case panel), from the left: the RCA block (centred at x 48, x 30.1–65.9 with its tabs) and the
  3.5 mm jack (x 72.3). These positions are constants at the top of `gen_pcb.py`; move them to suit the case.
- **DevKitC (U1):** two 1 × 19 female sockets (8.5 mm), rows 25.4 mm apart, lying along the card: pin 1 (3V3) at
  x 13.5, y 39.4, the J2 row (pins 1–19) at y 39.4 and the J3 row at y 14.0, from x 13.5 to 59.2. The DevKitC's PCB
  spans y ≈ 12.5–40.9; its **antenna end is at the left edge** and its **USB end points right** (x ≈ 61), over the
  DACs. It stands about 8.5 mm up on the sockets (module top ≈ 13.5 mm off the card, the USB plug higher), so low SMD
  parts sit under it. The module and its antenna reach past pin 1 toward the left edge: the footprint allows 13 mm
  (measure the -VE: Espressif's footprint shows the WROOM antenna 1.1–7.0 mm past pin 1), and the card keeps **no
  copper on either layer** from the left edge to x 12, y 12–41.5. Keep metal out of that end of the case. Housing: the
  card's left edge is board y 93, so the antenna is at y ≈ 81–93, z ≈ 22–55 (was the WROVER at the top corner).
- **RCA block (J2):** Kycon KLP42X-61-WW/RR (white on top, red below). The jacks face off the top edge (right-angle),
  and the plate's rear face is the board edge. The body is 34 mm wide and 10 mm deep behind the edge, and **26.5 mm
  tall** above the card (jack centres 6.7 and 20.7 mm up, columns 18 mm apart). The 1.0 mm plate and the barrels
  (7.8 mm) stand out past the edge. From the drawing (depths from the plate's front face): sleeve pins 1 / 4 at 7.0 mm,
  lower tips 3 / 6 at 10.25 mm, upper tips 2 / 5 at 11.5 mm, each column's pins 4 mm apart; two snap-in side tabs
  (2.4 mm wide, 7.5 mm deep, ±16.5 mm, barb to ±17.6 mm) in 1.9 × 3.0 mm slots, tied to ground. No switch contacts.
- **Mic jack (J6):** Same Sky SJ1-3533NG, through-hole, soldered by hand (KiCad's stock footprint, pins T / R / S),
  right of the block on the same edge. Its front face is on the edge (1.2 mm before the sleeve pin), body x 67.4–75.6
  and 14 mm deep behind the edge, its nose past the edge centred at x 72.3, where the SMT jack's was. It replaced the
  SJ-3523-SMT on 2026-11-19 so the card's connectors are all through-hole; the board was edited in place (footprint
  swapped, the jack's ground fan-outs dropped since its pins reach both pours, the tip re-routed on B to the old via).
- **Parts side (F):** every part. The DACs, ADC and op-amp sit right of the DevKitC's USB end (x 61–80), the CAN
  transceiver and the link buffer above the slot header, passives where the placer put them (some under the DevKitC).
- **Back (B):** copper and silkscreen only, no parts.
- **Assembly:** the fab places the SMD parts (one side). The through-hole parts (J1, J2, J6 and the two DevKitC sockets)
  are soldered by hand. They are marked **DNP** in the schematic and on the board (`DNP` in `gen_sch.py` /
  `gen_pcb.py`), so they stay out of the placement files, and the PCBWay BOM lists them as DNP lines
  ("do not place or buy").
- **Layers:** 2. F signal + GND pour, B GND pour + signal. 0.2 mm tracks, 0.3 mm on the supplies, 0.13 mm clearance,
  0.6 / 0.3 mm vias. Every SMD ground pin has its own via to the B pour; stitching vias tie the pours together.
- **Solder-mask dams:** U3 (VSSOP-8, 0.65 mm) has its pads narrowed to 0.43 mm so the gaps are 0.22 mm, enough for
  PCBWay to keep a mask dam (they rejected 0.15 mm on 2026-11-19). The TSSOPs are 0.25 mm as drawn. Run
  `python3 ../tools/mask_dams.py AudioCarrier` again after any regenerate (it is lost otherwise).

## Status

**Rev J (2026-11-19): the bench DevKitC-VE in sockets, 2 layers.** The WROVER-E module, the AP7361C regulator and
its capacitors (C1, C3), the module decoupling (C8, C9), the EN / BOOT parts (R2, R3, C10, SW1, SW2), the programming
header J10 and the unused node straps JP1 / JP2 are gone; the other reference numbers are unchanged. GPIO0's net is
now MCLK (was BOOT). Re-placed and re-routed by `gen_pcb.py --route` (GND left out of the router: the pours and
per-pin vias join it), then `mask_dams.py`, fill, DRC with the parity check: **0 errors, 0 unconnected**, 21
silkscreen warnings and U3's footprint-differs-from-library (the narrowed pads). 590 vias (64 GND fan-outs, 493
stitching), 109 track segments on B. Ten router vias 0.06–0.15 mm from an SMD pad were then nudged by hand (≤ 0.1 mm)
to ≥ 0.15 mm, PCBWay's via-off-pad rule; re-running `--route` does not reproduce that. Nothing in the antenna keep-out. The rev I notes that follow describe the
module version (in git).


**Schematic: done** (the slot card, 90 parts). **PCB: placed and routed** (2026-10-23, rev G): 80 × 60 mm, 4 layers, 90
placed parts (88 in the placement file: JP1 / JP2 are copper only). DRC with the schematic parity check: **no errors,
nothing unconnected, no footprint / parity errors**; 28 silkscreen warnings (text at the edge, over pads, overlapping) and one footprint-differs-from-library warning (U3, the mask-dam pads).
Rev H (2026-10-27): every part on F (single-sided assembly), re-placed and re-routed by `gen_pcb.py --route`
(120 passes); the ground stitching closed the one link Freerouting left. Two fan-out vias (R17 pad 2, R25 pad 1)
were then moved by hand to 0.15 mm clear of their pads, the PCBWay via-off-pad rule. 550 vias, none touching a pad;
nothing in the antenna keep-out. Fab outputs in `fab/`
(`python3 ../tools/fab_export.py AudioCarrier`); `fab/AudioCarrier_BOM_PCBWay.csv` is kept by hand and matches the
placement file; `AudioCarrier_BOM.csv` is the same list.
Rev I (2026-10-27): RCA block **Switchcraft PJRAS2X2S01X → Kycon KLP42X-61-WW/RR** (34 mm wide, 6 pins + 2 snap-in
tabs, 26.5 mm tall), centred at x 48 (was 52) so it clears the jack; re-placed and re-routed by `gen_pcb.py --route`.
Freerouting left /EN, /I2S_LRC and U8 pin 7 (GND) open and the finisher could not join them, so they were finished
by a one-off grid router (not in `generate/`): /EN a 5.5 mm F track by SW1; /I2S_LRC from U5 pin 15 through two vias
to U6's side, after lifting U5 pin 16's GND fan-out stub (pin 16 reaches the pours without it); U8 pin 7 a track and
a via to the plane. Ten Freerouting vias 0.137–0.146 mm from a pad were nudged (≤ 0.02 mm) to ≥ 0.15 mm, the PCBWay
via-off-pad rule. DRC with the parity check: **0 errors, 0 unconnected**, 28 silkscreen warnings; 546 vias, none
within 0.15 mm of a pad. Re-running `gen_pcb.py --route` does not reproduce these fixes. The finisher now checks
clearance only against items near each join (it hung for 20+ minutes on the full board).

- **Rev G (2026-10-23), before the PCBWay order:**
  - **Module:** ESP32-PICO-MINI-02-N8R2 (NRND) → **ESP32-WROVER-E-N8R8**. Footprint drawn from the datasheet v2.4
    (figure 15: 38 pads 1.5 × 0.9 on 1.27 mm, centres 17.5 mm apart, pin 1 7.44 mm from the antenna end, pin 19 1.1 mm
    from the far end; EPAD 3.7 mm square, 7.5 mm from the left edge and 16.16 mm from the far end, nine 0.9 mm paste
    openings, no vias in it). Pins checked against table 3. Same GPIOs as before (pin map above).
  - **Regulator:** AMS1117-3.3 + 22 µF tantalum → **AP7361C-33E-13** with ceramics (pinout 1 IN, 2 GND, 3 OUT, from
    the Diodes datasheet); tab solid to a ground copper area with 12 vias.
  - **Mic:** aux path removed (ring buffer, its coupling cap and bias resistor, the RING net); bias now from +5VA
    through 1k + 47 µF; ring and sleeve to ground; TLV9062 B a follower on VMID; PCM1808 VINR AC-grounded.
    Tinkersphere TS3328 → **Same Sky SJ-3523-SMT** (stock footprint, datasheet pinout). The SJ1-3523N was checked
    first: it is 12 mm wide, more than the 9.3 mm the housing allows there.
  - **Moved:** the module (now x 1.2–19.2, y 0.2–31.6), SW1 / SW2 (below the module, y 36), the jack (x 72.3); every
    passive re-placed by the script; re-routed. Fixed: outline, J1, J2, J10, the jack's edge position.
- **The RCA block's footprint** is drawn from Kycon's KLP42X-61-xx/xx drawing (series sheet, 2013; third-angle, so
  the bottom view is not mirrored and left / right are as seen from the plug side). The pin positions were measured
  off the vector drawing where it gives no dimension (the tabs' x, the pin order by depth). I took the plate's rear
  face as the board edge; if the plate should sit flush with the case panel instead, the edge moves 1.0 mm. Check the
  tab slots and the 1.4 mm pin holes with the block in hand. It replaced the Switchcraft PJRAS2X2S01X (27 mm wide,
  different pinout) on 2026-10-27, before the first order.
- **Earlier checks (2026-10-05), still valid:** PCM1808 (TI SLES177B pin table; MD0 / MD1 low = slave, FMT low = I2S;
  VREF 100n + 10u); PCM5102A (TI pin table; SCK low = internal PLL, FMT low = I2S, LDOO 100n as TI asks); MCP2562;
  TLV9062 (standard dual pinout; an OPA2376 until 2026-10-26, swapped for price, any RRIO dual with that pinout fits); 74LVC125 (1 / 4 / 10 / 13 OE, BCK / WS / ADC data / MCLK onto LINK0 / 1 / 2 / 4); J1
  against the slot pinout and DigiDash2's J31–J36; the RCA block against Kycon's drawing (2026-10-27).
- **BOM complete:** every line of `fab/AudioCarrier_BOM_PCBWay.csv` has a maker and part number.
- **Still open:**
  - **Measure the DevKitC-VE before ordering:** how far its module (antenna end) reaches past pin 1 (the footprint
    allows 13 mm) and its overall length to the USB end (taken as 47.3 mm from pin 1). The row spacing (25.4 mm) and
    pin order are Espressif's own footprint.
  - `docs/hardware/housing_envelope.md` still lists the WROVER module at the card's top corner and J10; with rev J the
    DevKitC spans card x ≈ 0.5–61, y 12.5–41 (board y ≈ 32–93, z ≈ 28–56), about 13.5 mm off the parts face plus
    the USB plug, its antenna at the card's left edge (board y ≈ 81–93). Update it and keep metal off that end.
  - The jack's panel opening: the SJ1-3533NG nose is centred at card x 72.3 as before; take its diameter and height
    off the card from Same Sky's SJ1-353xNG drawing before cutting the case.
  - The RCA plate edge line (above) and the card's 80 × 60 mm outline against the case.
  - Bench: the electret path (bias, gain, levels with a real car mic); the DevKitC's regulator temperature with the card's loads added.

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
- Program the DevKitC over its own USB, either out of the card or in it with the card unpowered (see Power). Its
  BOOT / EN buttons and auto-reset work as on the bench.
- It must be the **-VE** (WROVER-E, 8 MB PSRAM): the bridge keeps its album-art buffers in PSRAM; a PSRAM-less DevKitC
  (the -32 / WROOM) leaves Bluetooth ~32 KB of RAM while streaming. The firmware's `esp32wrover` / `audiocarrier`
  settings are the bench board's. Its flash uses GPIO6–11 and its PSRAM GPIO16 / 17: not connected here.
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
