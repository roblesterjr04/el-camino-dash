# Audio carrier

Accessory carrier for one DigiDash header position (`docs/hardware/accessory_ports.md`): a bare **ESP32-WROOM-32E**
module, a **Maxim MAX5134** quad 16-bit SPI DAC and four **face-mount RCA jacks** (front L / R, rear L / R). CAN node
`0x10`, 5 V from the port only.

| Files | |
| --- | --- |
| `AudioCarrier.kicad_sch` / `.kicad_pcb` / `.kicad_pro` | The board (2 layers, 100 × 60 mm, GND pour on both sides). **Placed, not routed**. Written for KiCad 7 file formats, opens in 7 to 10 |
| `AudioCarrier.pretty` | The RCA jack footprint (RCJ-02x, from the Same Sky datasheet). Everything else is a stock KiCad footprint |
| `generate/` | Scripts that produced the files (`gen_sch.py`, `gen_footprints.py`, `gen_pcb.py`, `drc.py`) |

## Signal path

```
ESP32 SPI (SCLK GPIO18, DIN GPIO19, CS GPIO5, LDAC GPIO21) -> MAX5134 OUT0..OUT3 (0 - 2.44 V, midscale 1.22 V)
   -> 2nd-order Sallen-Key low-pass (3.9k / 3.9k, 2.2n / 1n, about 27 kHz) -> 10u DC block -> 100 R -> RCA tip
```

| Channel | DAC pin | Op-amp | Jack | Colour / part |
| --- | --- | --- | --- | --- |
| Front left | OUT0 | U3 A | J2 | white, RCJ-023 |
| Front right | OUT1 | U3 B | J3 | red, RCJ-022 |
| Rear left | OUT2 | U4 A | J4 | white, RCJ-023 |
| Rear right | OUT3 | U4 B | J5 | red, RCJ-022 |

Full scale is about 0.86 Vrms. The DAC powers up at midscale (M/Z high), so there is no step into the DC-block caps.

## Status

Schematic done (netlist checked against the design; KiCad 7's CLI has no ERC, so run ERC once in KiCad). PCB placed with no
courtyard, clearance or edge errors in DRC. Autorouting with Freerouting 2.1 did not finish cleanly (it stalled at
26 unrouted connections and threw exceptions), so the board is left unrouted: open it in KiCad, then route (Freerouting
GUI or by hand) and refill zones.

## Things to know

- **The MAX5134 is a precision control DAC, not an audio DAC.** There is no I2S, no oversampling and no built-in
  reconstruction filter. The firmware has to stream 4 x 24-bit SPI frames at the sample rate (96 kHz per channel needs a
  9.2 MHz clock, DMA-queued, CS per frame) and should interpolate above 48 kHz so the 27 kHz low-pass can remove the
  images. That firmware does not exist yet; today's bridge sketch outputs I2S.
- On paper: noise 18 uVrms (1 Hz to 10 kHz, about 94 dB below full scale), INL +-8 LSB (roughly -78 dB worst case),
  5 us settling, 1.25 V/us slew. Fine for a car; bench it before trusting it.
- **Height.** The RCJ-02 jacks are 22.1 mm tall and face away from the board, so with the header stack the carrier
  needs roughly 35 mm behind the DigiDash board (the DevKit carrier needed about 20 mm). Check the dash cavity.
- The RCJ-02 colour code is the last digit: 2 = red, 3 = white. The DigiDash board's J11 / J12 use `RCJ-022` (red).
- 5 V analog is the port's 5 V through a ferrite; the ESP32 and the DAC's digital side run from an AMS1117-3.3
  (about 0.4 W at peak: give it copper).
- Program it over the 5-pin header J10 (GND, TX0, RX0, IO0, EN, 3.3 V logic) with the BOOT and RESET buttons.
  Do not power it from the adapter.
- The DevKit-and-MAX98357A version this replaced is in git history (commit "AudioCarrier: first-cut ...").

## Regenerating

KiCad 7 or newer with its stock libraries, plus Freerouting for the routing step:

```bash
python3 generate/gen_footprints.py && python3 generate/gen_sch.py
/usr/bin/python3 generate/gen_pcb.py [--route freerouting.jar]   # the python that has pcbnew
/usr/bin/python3 generate/drc.py
```
