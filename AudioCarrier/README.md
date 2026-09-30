# Audio carrier

A **double-bay** accessory carrier for the DigiDash rev 2 board: a whole bay, **50 × 52 mm**, with its female header at
bay position A and M3 holes at both A and B (`docs/hardware/accessory_ports.md`, rev 2 section). On it:
- an **ESP32-PICO-MINI-02** module (classic ESP32: A2DP + AVRCP + HFP)
- two **TI PCM5102A** stereo DACs (front pair, rear pair)
- a **TI PCM1808** ADC with a microphone preamp
- a **2 × 2 right-angle RCA block** (Switchcraft PJRAS2X2S01X) and, below it on the same right edge, a small
  **3.5 mm jack** (Tinkersphere TS3328) for the mic / aux input

CAN node `0x10`, 5 V from the port only. The rev 1 board has no bays; there, the carrier needs a header position with
room for 50 × 52 mm.

| Files | |
| --- | --- |
| `AudioCarrier.kicad_sch` / `.kicad_pcb` / `.kicad_pro` | The board: 50 × 52 mm, 4 layers, parts on both sides, **routed**. KiCad 10 |
| `AudioCarrier.pretty` | The ESP32-PICO-MINI-02 footprint (land pattern from the Espressif datasheet) and the RCA block (hole pattern from Switchcraft's PJRAS2X2S__X customer drawing), the 3.5 mm jack (from the drawing on Tinkersphere's TS3328 product page), and plain 3D models of both. Everything else is stock KiCad or `DigiDash_Accessory` |
| `generate/` | Scripts that produced the files (`gen_footprints.py`, `gen_sch.py`, `gen_pcb.py`, `drc.py`) |

## Signal paths

```
ESP32 I2S0 (master) BCK GPIO14, WS GPIO25, DOUT GPIO27 ──► PCM5102A front ──► 470 R + 2.2 nF ──► J2 FL, J3 FR
ESP32 I2S1 (slave, same BCK / WS)  DOUT GPIO26            ──► PCM5102A rear  ──► 470 R + 2.2 nF ──► J4 RL, J5 RR
J6 tip, mic (2.2k bias) ─ 1u ─ OPA2376 (gain 22) ─ 1u ─┐
J6 ring, aux in ─────── 1u ─ OPA2376 (unity)   ─ 1u ─┴► PCM1808 ──► ESP32 I2S0 DIN GPIO35
ESP32 GPIO0 (CLK_OUT) 12.288 MHz = 256 fS master clock ──► PCM1808 SCKI      GPIO4 = DAC mute (XSMT, pulled low)
```

| Jack (J2, the block) | Signal | Insert colour |
| --- | --- | --- |
| Left column, top | Front left (U5 OUTL) | white |
| Left column, bottom | Front right (U5 OUTR) | red |
| Right column, top | Rear left (U6 OUTL) | white |
| Right column, bottom | Rear right (U6 OUTR) | red |
| J6, 3.5 mm jack (Tinkersphere TS3328) | tip = mic, ring = aux in, sleeve = GND (**pin functions provisional**, see below) | right edge, below the block |

"Left" and "right" are seen from the plug side, standing outside the right edge. The sleeves and the front plate's two
solder tabs are ground.

- **DACs:** 3-wire I2S, SCK tied to ground so the internal PLL runs from BCK (BCK must be 64 fS, which is what the ESP32
  produces with 32-bit slots). FMT, FLT and DEMP low. Outputs are ground-centred 2.1 Vrms: no DC-block caps, just the
  datasheet's 470 R / 2.2 nF filter. The two DACs share BCK and LRCK and have their own data lines, so front and rear are
  independent (fader in software). Both stay muted until the firmware raises GPIO4. Decoupling as in the datasheet
  (LDOO 0.1 µF).
- **ADC:** slave mode (MD0 = MD1 = low), I2S 24-bit, 5 V analog / 3.3 V digital, full scale 3 Vp-p. It runs from the same
  BCK / LRCK as the DACs, so ESP32 I2S0 does full duplex (play + record) in master mode.
- **Mic input:** electret bias 2.2k from filtered 3.3 V, RF cap at J2, DC-blocked; preamp gain 1 + 47k/2.2k = 22
  (27 dB), biased at 2.5 V. Aux is a second, unbiased line-level input (unity buffer).
- **Power:** +5 V from the port; AMS1117-3.3 for the ESP32 and digital sides (22 µF tantalum on its output, 10 µF +
  100 nF at the module); ferrites make +5VA (ADC analog, op-amp) and +3V3A (DAC analog and charge pump).

## Board

- **Outline:** 50 × 52 mm, a whole bay.
  - The top half is bay position A: J1 (`Carrier_Header_2x05`) with its M3 holes MP1 / MP2.
  - The bottom half is position B. Its lower-left M3 hole (H1) is used. **Its lower-right hole is not**: the RCA block's
    body stands there. The carrier is held by three standoffs, and the block's own screw hole (in the middle of the four
    jacks) fastens it to the case panel, so plug forces go to the case and not the board.
  - The holes that are used are plated and tied to GND. Nothing else is placed within 3.3 mm of one.
- **Pocket:** the underside keeps a 21.5 × 10 mm area over B's shrouded header (x 14.25–35.75, y 37.8–47.8) free of
  parts and through-hole pins.
- **RCA block (J2):** on the right edge, centred 27.2 mm from the top. The jacks face off the board (right-angle), and the
  plate's rear face is the board edge. The body is 27 mm wide and 13.7 mm deep behind the edge, and 28.5 mm tall. The
  barrels (8.2 mm) and the 1.5 mm plate stand out past the edge. From the drawing: tip pins 12.5 mm behind the plate,
  sleeve pins 5.25 mm, plate tabs 8.75 mm, and 28.5 mm total height.
- **Mic / aux jack (J6):** on the same edge, below the block (centre 46.6 mm from the top). It is 12 × 7 mm with the barrel
  standing 2.5 mm past the edge, so it fits in the 10 mm left under the block (about 0.3 mm between courtyards).
- **J6 pin functions are provisional.** The vendor drawing gives the hole pattern but numbers the pins 2, 3, 4, 4, 5
  without naming them, and its wiring diagram looks 4-pole (the listing's title says 3-pole). The board uses: pin 2 = tip
  (mic), pin 4 (two tabs) = ring (aux), pins 3 and 5 = sleeve (ground). Before ordering, plug a 3.5 mm plug into one and
  check continuity from the plug's tip, ring and sleeve to each pin. If it differs, edit `JACK_MAP` in `gen_sch.py` and
  re-run (a few minutes).
- **Top** (away from the DigiDash board): the module, regulator, CAN transceiver, buttons, J10, the RCA block and the
  3.5 mm jack.
- **Bottom** (the standoff gap, facing the DigiDash board): both DACs, the ADC, the op-amp and their passives, all
  under 1.2 mm tall, plus the J1 socket. The 11 mm stack clears them.
- **Layers:** F signal, In1 solid GND, In2 signal / GND fill, B signal. 0.2 mm tracks, 0.4 mm on the supplies,
  0.15 mm clearance, 0.6 / 0.3 mm vias: standard 4-layer rules at JLCPCB / PCBWay.
- **Antenna:** the module's antenna end sits on the left edge, with a keep-out (no copper, any layer) under it and no
  parts under it on the bottom. MP1's standoff is 0.3 mm below the antenna's corner. It is fine for a phone in the cabin;
  do not put metal right against that edge.
- **Height:** the RCA block is 28.5 mm tall above the carrier (two jacks stacked, lying on their sides), so with the
  11 mm stack the carrier needs about 40 mm behind the DigiDash board. Check the dash cavity.
- **Which edge:** the right edge, when the carrier is in bay 1, faces the short edge of the DigiDash board opposite the
  sensor plugs (J3 / J10), 8 mm away. The barrels stand out 8.2 mm past the carrier's edge, so the plugs come out about
  at that board edge. In bay 2 that edge faces the bay power plug instead.

## Status

**Routed** (the double-bay version).
- DRC (with the schematic parity check, `generate/drc.py`): no errors, 0 unconnected.
- What is left is silkscreen warnings (text next to pads, the module outline at the edge) and two parity notes for
  J1's MP1 / MP2. The M3 holes are GND on the board, and the 2×5 header symbol has no pins for them.
- About 700 track segments and 270 vias.

- Every ground pin except the module's has its own via to the In1 plane, placed before routing; the pours on the other
  three layers are stitched to it. The module's ground pads are one pad group in its footprint (they are joined
  inside the module), so its small corner pads need no copper of their own.
- The schematic has had ERC: no errors (only off-grid warnings from the generated layout, and the embedded
  `AudioCarrier:` symbols not being in a library table).
- **The RCA block's footprint** is drawn from Switchcraft's customer drawing
  (`https://switchcraft.com/assets/1/24/pjras2x2s__x_series_cd.pdf`; it is proprietary, so it is not copied here). One
  thing the drawing does not say: which line is the PCB edge. I took the dashed line (the plate's rear face) as the
  edge. If the plate should sit flush with the case panel instead, the board edge moves 1.5 mm (the plate thickness),
  and the carrier's right edge goes 1.5 mm further out. Check with the block in hand and against the case panel cut-out,
  and confirm the 3D model's height (28.5 mm) and the panel's mounting screw position (the middle of the four jacks).
- Check before ordering: the ESP32-PICO-MINI-02 land pattern against the module you buy, the standoff height
  (11 mm stack) against the bottom-side parts (≤ 1.2 mm), and that nothing on the DigiDash board's back sits inside
  this carrier's footprint at the position you use.

## Things to know

- **Firmware is not written yet.** This board needs: I2S0 master with MCLK on GPIO0 (12.288 MHz at 48 kHz) and a
  data-in pin (GPIO35); I2S1 as a slave on the same BCK / WS routed through the GPIO matrix for the rear DAC; GPIO4
  raised to un-mute; and the HFP client's PCM audio wired to the mic path. A2DP / AVRCP / HFP are all supported by the
  classic ESP32 stack.
- **Node straps moved:** the PICO-MINI-02 has no GPIO23, so the second strap (JP2) is on **GPIO22**. GPIO13 (JP1) is
  unchanged. `docs/hardware/accessory_ports.md` still says GPIO13 / GPIO23 for the DevKit.
- **GPIO0 is the master clock and the BOOT strap.** It must be high (or free) at reset. The PCM1808 SCKI input is 5 V
  tolerant and just sits on it; the 33 R series resistor damps the clock. Pressing BOOT still enters the loader.
- Program it over the 5-pin header J10 (GND, TX0, RX0, IO0, EN, 3.3 V logic) with the BOOT (SW2) and RESET (SW1)
  buttons. Power it from the port, not the adapter.
- Order the module as **ESP32-PICO-MINI-02-N8R2** (8 MB flash, 2 MB PSRAM). The PSRAM is needed: the bridge keeps its
  album-art buffers there (build with `BOARD_HAS_PSRAM`, as the `esp32wrover` envs do); a PSRAM-less part would leave
  Bluetooth ~32 KB of RAM while streaming. Its flash and PSRAM use GPIO6–11 and GPIO16 / 17
  internally; none of those are used here.
- History (in git): the 50 × 26 mm single-position version (jacks on a JST PH pigtail), the 104 × 70 mm WROOM-32E
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
