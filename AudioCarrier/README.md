# Audio carrier

Accessory carrier for one DigiDash header position (`docs/hardware/accessory_ports.md`): a bare **ESP32-WROOM-32E**
module (A2DP + AVRCP + HFP), two **TI PCM5102A** stereo DACs (front pair, rear pair), a **TI PCM1808** ADC with a
microphone preamp and a 3.5 mm TRS jack, and four **face-mount RCA jacks**. CAN node `0x10`, 5 V from the port only.

| Files | |
| --- | --- |
| `AudioCarrier.kicad_sch` / `.kicad_pcb` / `.kicad_pro` | The board (2 layers, 104 × 70 mm). Written in KiCad 7 file formats, opens in 7 to 10 |
| `AudioCarrier.pretty` | The RCA jack footprint (RCJ-02x, from the Same Sky datasheet). Everything else is a stock KiCad footprint |
| `generate/` | Scripts that produced the files (`gen_sch.py`, `gen_footprints.py`, `gen_pcb.py`, `drc.py`) |

## Signal paths

```
ESP32 I2S0 (master) BCK GPIO14, WS GPIO25, DOUT GPIO27 ──► PCM5102A front ──► 470 R + 2.2 nF ──► J2 FL (white), J3 FR (red)
ESP32 I2S1 (slave, same BCK / WS)  DOUT GPIO26            ──► PCM5102A rear  ──► 470 R + 2.2 nF ──► J4 RL (white), J5 RR (red)
J6 TRS  tip: electret mic (2.2k bias) ─ 1u ─ OPA2376 (gain 22) ─ 1u ─┐
        ring: aux in ────────────────── 1u ─ OPA2376 (unity)  ─ 1u ─┴► PCM1808 ──► ESP32 I2S0 DIN GPIO35
ESP32 GPIO0 (CLK_OUT) 12.288 MHz = 256 fS master clock ──► PCM1808 SCKI      GPIO4 = DAC mute (XSMT, pulled low)
```

- **DACs:** 3-wire I2S, SCK tied to ground so the internal PLL runs from BCK (BCK must be 64 fS, which is what the ESP32
  produces with 32-bit slots). FMT, FLT and DEMP low. Outputs are ground-centred 2.1 Vrms: no DC-block caps, just the
  datasheet's 470 R / 2.2 nF filter. The two DACs share BCK and LRCK and have their own data lines, so front and rear are
  independent (fader in software). Both stay muted until the firmware raises GPIO4.
- **ADC:** slave mode (MD0 = MD1 = low), I2S 24-bit, 5 V analog / 3.3 V digital, full scale 3 Vp-p. It runs from the same
  BCK / LRCK as the DACs, so ESP32 I2S0 does full duplex (play + record) in master mode.
- **Mic input:** tip is the microphone (2.2k bias from filtered 3.3 V, RF cap, DC-blocked), preamp gain 1 + 47k/2.2k = 22
  (27 dB), biased at 2.5 V. Ring is a second, unbiased line-level input (unity buffer). Sleeve is ground.
- **Power:** +5 V from the port; AMS1117-3.3 for the ESP32 and digital sides; ferrites make +5VA (ADC analog, op-amp) and
  +3V3A (DAC analog and charge pump). About 0.4 W peak in the regulator: give it copper.

| Channel | DAC | Jack | Part (Same Sky RCJ-02x, last digit = colour) |
| --- | --- | --- | --- |
| Front left | U5 OUTL | J2 white | RCJ-023 |
| Front right | U5 OUTR | J3 red | RCJ-022 |
| Rear left | U6 OUTL | J4 white | RCJ-023 |
| Rear right | U6 OUTR | J5 red | RCJ-022 |

Pinouts of the PCM5102A and PCM1808 are from the datasheets in this project's chat (TSSOP-20 / TSSOP-14 `PW` packages);
the RCJ-02 pads are from its datasheet. The DigiDash board's J11 / J12 use `RCJ-022` (red).

## Status

Schematic done and its netlist checked. KiCad 7's command line has no ERC, so run ERC once in KiCad. PCB placed with no
courtyard, clearance or edge errors in DRC (only silkscreen warnings remain). **Not routed**: route it in KiCad. A trial
run of Freerouting 1.9 completed all but one connection (I2S_LRC to U5 pin 15), so the placement routes; Freerouting 2.1
crashes on this board. Run **Update PCB from Schematic** first if you change the schematic.

## Things to know

- **Firmware is not written yet.** Today's bridge sketch drives one I2S DAC on GPIO25 / 26 / 27. This board needs: I2S0
  master with MCLK on GPIO0 (12.288 MHz at 48 kHz) and a data-in pin (GPIO35); I2S1 as a slave on the same BCK / WS
  routed through the GPIO matrix for the rear DAC; GPIO4 raised to un-mute; and the HFP client's PCM audio wired to the
  mic path. A2DP / AVRCP / HFP are all supported by the classic ESP32 stack.
- **GPIO0 is the master clock and the BOOT strap.** It must be high (or free) at reset. The PCM1808 SCKI input is 5 V
  tolerant and just sits on it; the 33 R series resistor damps the clock. Pressing BOOT still enters the loader.
- **Height.** The RCJ-02 jacks are 22.1 mm tall and face away from the board, so with the header stack the carrier needs
  roughly 35 mm behind the DigiDash board (the DevKit carrier needed about 20 mm). Check the dash cavity. The TRS jack is
  the CUI SJ1-3523N right-angle part from KiCad's library, sitting in a notch on the bottom edge.
- Program it over the 5-pin header J10 (GND, TX0, RX0, IO0, EN, 3.3 V logic) with the BOOT and RESET buttons. Do not power
  it from the adapter.
- History: the DevKit + MAX98357A version and the MAX5134 version this replaced are in git history.

## Regenerating

KiCad 7 or newer with its stock libraries, plus Freerouting for the routing step:

```bash
python3 generate/gen_footprints.py && python3 generate/gen_sch.py      # also writes generate/refs.json (part groups for placement)
/usr/bin/python3 generate/gen_pcb.py [--route freerouting.jar]         # the python that has pcbnew
/usr/bin/python3 generate/drc.py
```
