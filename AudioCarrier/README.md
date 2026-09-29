# Audio carrier

Accessory carrier for one DigiDash header position (`docs/hardware/accessory_ports.md`), sized to the **50 × 26 mm
envelope** of a single position: an **ESP32-PICO-MINI-02** module (classic ESP32: A2DP + AVRCP + HFP), two **TI
PCM5102A** stereo DACs (front pair, rear pair), a **TI PCM1808** ADC with a microphone preamp, and one **JST PH 9-pin**
connector that carries the four line outputs and the mic / aux inputs to panel-mount jacks on a pigtail. CAN node
`0x10`, 5 V from the port only.

| Files | |
| --- | --- |
| `AudioCarrier.kicad_sch` / `.kicad_pcb` / `.kicad_pro` | The board: 50 × 26 mm, 4 layers, parts on both sides. KiCad 10 |
| `AudioCarrier.pretty` | The ESP32-PICO-MINI-02 footprint (land pattern from the Espressif datasheet). Everything else is stock KiCad or `DigiDash_Accessory` |
| `generate/` | Scripts that produced the files (`gen_footprints.py`, `gen_sch.py`, `gen_pcb.py`, `drc.py`) |

## Signal paths

```
ESP32 I2S0 (master) BCK GPIO14, WS GPIO25, DOUT GPIO27 ──► PCM5102A front ──► 470 R + 2.2 nF ──► J2-1 FL, J2-2 FR
ESP32 I2S1 (slave, same BCK / WS)  DOUT GPIO26            ──► PCM5102A rear  ──► 470 R + 2.2 nF ──► J2-4 RL, J2-5 RR
J2-7 mic (2.2k bias) ─ 1u ─ OPA2376 (gain 22) ─ 1u ─┐
J2-8 aux in ────────── 1u ─ OPA2376 (unity)   ─ 1u ─┴► PCM1808 ──► ESP32 I2S0 DIN GPIO35
ESP32 GPIO0 (CLK_OUT) 12.288 MHz = 256 fS master clock ──► PCM1808 SCKI      GPIO4 = DAC mute (XSMT, pulled low)
```

| J2 pin | Signal | Panel jack |
| --- | --- | --- |
| 1 | Front left | RCA, white |
| 2 | Front right | RCA, red |
| 3 | GND | front RCA sleeves |
| 4 | Rear left | RCA, white |
| 5 | Rear right | RCA, red |
| 6 | GND | rear RCA sleeves |
| 7 | Mic | 3.5 mm TRS tip |
| 8 | Aux in | 3.5 mm TRS ring |
| 9 | GND | 3.5 mm TRS sleeve |

Use shielded cable (or a twisted pair per channel with its ground) for the pigtail, and keep the mic lead on its own.

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

- **Outline** 50 × 26 mm, the envelope of one header position, centred on the two M3 holes of `Carrier_Header_2x05`
  (J1). The holes are the header's MP1 / MP2 (plated, GND). Keep the corner standoffs and screw heads within 3.3 mm of
  the hole centres: nothing else is placed there.
- **Top** (away from the DigiDash board): module, regulator, CAN transceiver, buttons, J2 and J10. **Bottom** (the
  standoff gap, facing the DigiDash board): both DACs, the ADC, the op-amp and their passives, all under 1.2 mm tall,
  plus the J1 socket. The 11 mm stack clears them.
- **Layers:** F signal, In1 solid GND, In2 signal / GND fill, B signal. 0.2 mm tracks, 0.4 mm on the supplies,
  0.15 mm clearance, 0.6 / 0.3 mm vias: standard 4-layer rules at JLCPCB / PCBWay.
- **Antenna:** the module's antenna end sits on the left edge, with a keep-out (no copper, any layer) under it and no
  parts under it on the bottom. MP1's standoff is 0.3 mm below the antenna's corner. It is fine for a phone in the cabin;
  do not put metal right against that edge.
- **Height:** the module is 2.4 mm, J2 (vertical PH) about 6 mm, and the pigtail plugs in from above.

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
- Order the module as **ESP32-PICO-MINI-02-N8R2** (8 MB flash, 2 MB PSRAM). Its flash and PSRAM use GPIO6–11 and GPIO16 / 17
  internally; none of those are used here.
- History: the 104 × 70 mm version (WROOM-32E, four RCA jacks and a TRS jack on the board), the MAX5134 version and the
  DevKit + MAX98357A version are in git history.

## Regenerating

KiCad 10 with its stock libraries, numpy in KiCad's python (`$KIPY -m pip install --user numpy`), and Freerouting 2.1
(Java 21) for the routing step:

```bash
KIPY=/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3
python3 generate/gen_footprints.py && python3 generate/gen_sch.py   # also writes generate/refs.json (part groups for placement)
$KIPY generate/gen_pcb.py --route freerouting-2.1.0.jar              # place, route, fill
$KIPY generate/drc.py
```
