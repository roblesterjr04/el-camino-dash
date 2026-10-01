# AM/FM radio card

A **slot card** for the DigiDash rev 2 board (80 × 60 mm, standing on edge in a 2×15 slot, any slot). Spec:
[`docs/hardware/radio_card.md`](../docs/hardware/radio_card.md). On it:
- a **Si4735-D60-GU** AM/FM/SW/LW tuner with RDS (SSOP-24), controlled over I2C
- an **STM32G0B1KBU6** (FDCAN) with an 8 MHz crystal, an **MCP2562** CAN transceiver and an **AP2112K-3.3** from the slot's 5 V
- a **74LVC1G125** that puts the tuner's digital audio on the slot I2S rail's RET line, only after the audio card grants it
- an **SMA** antenna jack, J3 SWD and J4 debug serial, two LEDs, a 32.768 kHz oscillator for the tuner's RCLK

**Status: first-draft schematic only.** No board yet. Not checked in KiCad's ERC (the environment that made it had KiCad 7, whose
CLI has no ERC); its netlist was exported with `kicad-cli` and matches the generator's netlist (`generate/netlist.json`).

| Files | |
| --- | --- |
| `RadioCard.kicad_sch` / `.kicad_pro` | The schematic (A2), KiCad 7 format, opens in 7–10 |
| `RadioCard_BOM.csv` | Parts list (values and footprints; parts marked PROVISIONAL in the schematic have no part number yet) |
| `generate/gen_sch.py` | Makes the schematic and `refs.json` / `netlist.json`. Copied from `GPSCard/generate/`. Needs KiCad's stock symbols (`KICAD_SYMBOLS=/usr/share/kicad/symbols`) |

## Provisional items (need AN332 / AN383, or a part choice)

- **RCLK:** the tuner's crystal oscillator shares the DCLK pin, so a 32.768 kHz **oscillator** feeds RCLK (U4, part not chosen,
  placeholder footprint). AN332 table 6 may allow the rail's 12.288 MHz MCLK instead.
- **Antenna:** FM through 1 nF, AM through 0.47 µF with a loopstick on J5, as in the data sheet. A car whip on AM needs
  AN383's network: JP2 (open) is the place for it. The SMA (Amphenol 132289 edge mount) and the ESD diode D2 are placeholders.
- **I2C address:** JP1 straps SEN; 0x11 / 0x63 is from memory of AN332.
- **MCU pins:** I2C1 on PB6 / PB7 (AF6), FDCAN on PA11 / PA12 (AF9) as on the GPS card: check against DS13560 before the board.
- **Sample-rate lock** between the tuner's reference and the audio card's frame clock: bench test.

## Regenerating

```bash
KICAD_SYMBOLS=/usr/share/kicad/symbols python3 generate/gen_sch.py
```
