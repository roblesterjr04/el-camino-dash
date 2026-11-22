# AI voice card

A **slot card** for the DigiDash rev 2 board: **80 × 60 mm**, standing on edge in a 2×15 slot, its right-angle header on the
bottom edge (`docs/hardware/accessory_ports.md`, rev 2 section). Requirements and design:
[`docs/hardware/ai_card.md`](../docs/hardware/ai_card.md). Firmware: [`firmware/ai_card/`](../firmware/ai_card/README.md)
(this card's ESP32-S3) and [`firmware/ai_agent/`](../firmware/ai_agent/README.md) (the assistant on the compute module).
It goes in **any slot**. On it:
- an **ESP32-S3-WROOM-1U-N16R8** (16 MB flash, 8 MB octal PSRAM; the IPEX version, because the radio is not used): CAN node,
  the I2S rail as a slave (microphone in, voice out), the wake word, the compute module's power switch
- a **2 × 20 female socket** (2.54 mm, 8.5 mm insulator) for the **compute module**, a Raspberry-Pi-header Linux board
  (reference: Radxa Zero 3W 8 GB with eMMC), held by two M2.5 standoffs
- a **TPS54560B buck** making the module's 5 V from **AUX 12 V** (slot pins 25–30), switched by the ESP32 through a P-FET
  (fail-safe: off whenever the ESP32 is off), with 470 µF bulk and a 220k / 47k sense of the AUX 12 V input
- a **74LVC125** that drives the rail's **RET** line only while the audio card grants it (enable pulled to *off*), 33 Ω in series
- an **MCP2562** CAN transceiver and an **AP7361C-33** 3.3 V regulator, both from the slot's 5 V
- **USB-C** (left edge) to the ESP32-S3's native USB, **BOOT** and **RESET** buttons, a **debug serial** header, two LEDs
  (module running, microphone open) and a **10k NTC** read by the ESP32

CAN node from its slot (`0x14 + (slot − 1)`), product `0x0007`. Firmware: `firmware/ai_card/`.

> ## Measure the compute module before ordering
>
> The module's size, header position and mounting holes on this card are **provisional**: they follow the Raspberry Pi Zero
> format, not a module in hand. The header map itself is the Raspberry Pi standard. Check, and edit the constants,
> then re-run the generators (see [Regenerating](#regenerating)):
>
> 1. **Pin 1 end and orientation of the header.** `J2_PIN1` in `generate/gen_pcb.py` (pin 1 at the left end, odd pins in the
>    upper row). If the module's pin 1 is at the other end the 5 V pins (2, 4) move to the other side of the card.
> 2. **Module outline** `MOD_X0, MOD_Y0, MOD_W, MOD_H` (65 × 30 mm, header 5 mm in from its lower edge). Nothing taller than
>    about 7 mm may stand inside it (the 470 µF is outside); check the module's bottom-side parts against the 8.5 mm socket.
> 3. **Mounting holes** `MOUNT_HOLES` (two of the module's far corners, 3.5 mm in; M2.5, 2.7 mm holes). Fit the standoffs
>    before judging: a 2×20 socket alone will walk out of a vibrating dash.
> 4. **Which header pins the module really uses** (`HEADER_NETS` in `generate/gen_sch.py`): UART on 8 / 10, ALIVE on 29,
>    SHUTDOWN_REQ on 31, ATTN on 33. Check that the module **accepts its 5 V supply on header pins 2 / 4** (it does on a Pi
>    Zero; confirm for the module in hand, and that nothing else powers it: no USB-C power while it sits on the card), which UART
>    pins 8 / 10 are on it and the device-tree overlay that enables it (`[link] port` in the agent's config), and that the GPIOs are
>    3.3 V.
> 5. **Height:** 8.5 mm socket + module + its heatsink must stay under the **20 mm** parts-side limit.
> 6. **Power:** the module's idle and peak current from the bench (the buck is 5 A; the design assumes 3 A peak).

| Files | |
| --- | --- |
| `AICard.kicad_sch` / `.kicad_pcb` / `.kicad_pro` | The card: 80 × 60 mm, 4 layers, parts on the front only. Generated with KiCad 7.0.11; the files were upgraded to KiCad 10 format on 2026-11-22 |
| `AICard_BOM.csv` | Bill of materials (`generate/gen_bom.py`) |
| `generate/` | `gen_sch.py`, `gen_pcb.py`, `gen_bom.py`, `drc.py`, `sexp.py`; `refs.json` is written by `gen_sch.py`. Copied from `LTECard/generate/` and adapted. Every footprint is stock KiCad, so there is no `.pretty` library |

## What's on it

```
AUX 12 V (pins 25-27) ─ 3 × 2.2 µF 100 V ─ TPS54560B (400 kHz) ─ 6.8 µH ─ MOD_5V ─┬─ 3 × 47 µF
AUX GND  (pins 28-30) ─────── all of the buck's grounds ─┘                         └─ 470 µF ─ socket pins 2, 4 (5 V)
                              socket GND pins (6, 9, 14, 20, 25, 30, 34, 39) = AUX_GND
AUX 12 V ─ 220k / 47k ─ GPIO5 (AUX present: the buck's INPUT; its output is off until the firmware has seen this)
Enable:  AUX12 ─ Q1 (P-FET) ─ 442k / 90.9k divider (starts at ~6.5 V, 5.1 V zener) ─ buck EN
         Q1 gate: 47k to AUX12, 47k to Q2; Q2 (N-FET) gate: GPIO11 through 1k, 100k pull-down. ESP32 off / in reset / unpowered = buck off
+5 V (pins 1, 2) ─ AP7361C-33 ─ +3V3 ─ ESP32-S3, MCP2562 VIO, 74LVC125, thermistor divider
ESP32-S3 UART1  TX GPIO41 ─ 1k ─► module RXD (socket 10)     RX GPIO42 ◄─ 1k ─ module TXD (socket 8)
         SHUT GPIO12 ─ 1k ─► socket 31   ALIVE GPIO13 ◄─ 1k ─ socket 29   ATTN GPIO14 ◄─► 1k ─ socket 33   (100k pull-downs on the ESP32 side)
I2S rail (slave): slot 15 BCK ─ 100 R ─ GPIO6   16 WS ─ 100 R ─ GPIO7   17 MIC ─ 100 R ─ GPIO15
         GPIO16 (RET data) ─► 74LVC125 gate 1 (/OE = GPIO17, 100k pull-up: off at power-up) ─ 33 R ─► slot 18 RET
CAN: GPIO9 TX / GPIO10 RX ── MCP2562 (VDD 5 V, VIO 3.3 V, STBY low, no termination) ── slot pins 5 / 6
Slot ID (pin 11): 10k to 3.3 V here, the slot's resistor to GND on the main board ──► GPIO4 (ADC1)
USB-C D+ / D- ── USBLC6-2SC6 ── GPIO20 / GPIO19 (native USB); CC1 / CC2 5.1k to GND; VBUS only to the clamp
```

### ESP32-S3 pins

| GPIO | Function | GPIO | Function |
| --- | --- | --- | --- |
| 41 / 42 | UART1 TX / RX to the module | 9 / 10 | CAN TX / RX (through 1k) |
| 11 | BUCK_ON (module's buck enable) | 4 | Slot ID (ADC1_CH3) |
| 12 | SHUTDOWN_REQ out | 5 | AUX 12 V present (ADC1_CH4): 2.1 V at 12 V |
| 13 | ALIVE in | 8 | NTC (ADC1_CH7) |
| 14 | ATTN (spare) | 1 / 2 | LED module / LED microphone |
| 6 / 7 / 15 | rail BCK / WS / MIC in | 19 / 20 | USB D− / D+ |
| 16 / 17 | RET data out / RET buffer enable (low = drive) | 43 / 44 | TXD0 / RXD0, debug serial J3 |
| 0 | BOOT button (strap) | EN | RESET button, 10k / 1 µF |

Not used: the strapping pins GPIO3, 45, 46 (and GPIO0 except for BOOT), the flash and octal-PSRAM pins GPIO26–37, GPIO18, 21, 38–40, 47, 48.
`firmware/ai_card/src/ai_pins.h` repeats this table; keep them in step.

### Grounds: AUX GND is not GND

As on the LTE card: on the main board AUX GND meets GND only at NT1, so this card does **not** join them. The buck, the 470 µF and the
module's return are on AUX_GND (its own pours on F, In2 and B over the buck area); the ESP32, CAN, USB, the rail and the 3.3 V
regulator are on GND (In1 is a solid GND plane under the whole card). Only logic crosses, through 1k resistors. On the bench a card
powered without the main board needs AUX GND and GND joined at the supply.

## Slot connector (J1)

| Pins | Signal | Used |
| --- | --- | --- |
| 1 / 2 | +5 V | ESP32-S3, CAN, 3.3 V regulator |
| 3 / 4, 7, 19 / 20 | GND | logic ground |
| 5 / 6 | CANH / CANL | MCP2562 |
| 11 | slot ID | 10k to 3.3 V, GPIO4 |
| 15 / 16 / 17 | I2S BCK / WS / MIC | inputs through 100 Ω |
| 18 | I2S RET | 74LVC125 and 33 Ω: a 3-state output, never driven without the audio card's grant |
| 25 / 26 / 27 | AUX 12 V | the module's buck |
| 28 / 29 / 30 | AUX GND | the buck and the module's return |
| 8, 9, 10, 12–14, 21–24 | switched / constant 12 V, 3.3 V, WAKE, LAMP, REV, audio bus, MCLK | not connected |

| Other connectors | | |
| --- | --- | --- |
| J2 | Compute module, 2 × 20 female, 8.5 mm | Raspberry Pi 40-pin map: 5 V on 2 / 4, GND on 6 / 9 / 14 / 20 / 25 / 30 / 34 / 39, UART 8 / 10, ALIVE 29, SHUTDOWN_REQ 31, ATTN 33 |
| J3 | debug serial, 1 × 3 right angle, left edge | 1 GND, 2 TX0 (to the adapter's RX), 3 RX0. 3.3 V logic, not a supply |
| J4 | USB-C (GCT USB4105, horizontal), left edge | native USB: flash and serial console. The card is powered from the slot |
| SW1 / SW2 | RESET / BOOT, top edge | Flash: hold BOOT, tap RESET (only needed if the USB-JTAG is not answering) |
| H1 / H2 | M2.5 holes for the module's standoffs | provisional positions |

## Board

- **Outline:** 80 × 60 mm, no card-mounting holes (the slot and the case hold it); two M2.5 holes for the module only.
- **Slot header (J1):** placed exactly as on the audio and LTE cards (`PinHeader_2x15_P2.54mm_Horizontal`, pin 1 17.78 mm right of
  the centre, the pins counting to the left, its body on the bottom edge); `gen_pcb.py` checks the orientation from the pads.
- **Middle:** the module's socket across the card (pin 1 at the left); the module hangs over the upper half of the card,
  8.5 mm up. **Under it** (all lower than 7 mm): the ESP32-S3-1U at the left, the buttons and thermistor, LEDs, resistors.
- **Left edge:** LEDs, USB-C (plug facing out), the debug header (pins out of the edge). **Top edge:** RESET / BOOT.
- **Bottom:** the buck by the AUX pins (25–30), the 470 µF beside it (10 mm tall, so outside the module's shadow), the RET
  buffer by the rail pins (15–18), the CAN transceiver and the 3.3 V regulator by pins 1–6.
- **Back:** nothing (it faces the next card; the 2.5 mm allowance is not used).
- **Layers:** F signal, In1 solid GND plane, In2 signal / pours, B signal. 0.2 mm tracks, 0.13 mm clearance, 0.6 / 0.3 mm vias.
  Net classes as on the LTE card: Power 0.3 mm (GND, +5V, +3V3); Aux 0.8 mm / 0.2 mm (AUX12); Mod5V 1.0 mm / 0.2 mm (MOD_5V); AuxGnd 0.8 mm / 0.2 mm
  (AUX_GND); Switch 1.0 mm / 0.2 mm (BUCK_SW).
- **No antenna:** the -1U module's IPEX connector is left empty; Wi-Fi is the compute module's (its own antenna).

## Status

**Schematic: done** (78 parts). **PCB: placed and routed** by `gen_pcb.py` (2026-10-04): 80 × 60 mm, 4 layers, 78 footprints,
747 tracks and 635 vias (558 of them GND stitching), Freerouting 2.4.1 plus the script's own steps, no hand edits.
DRC (KiCad 7.0.11): **0 errors, 0 unconnected.** Warnings only: 78 "library not in the library table" (the project has no
`fp-lib-table`, as on the LTE card), 20 silkscreen past the edge (J1, J3 and J4 stick out of the outline on purpose), 3 silkscreen
clipped by mask, 2 silkscreen overlaps. **Not ordered.** Merged to main on 2026-11-22; the BOM CSV was regenerated then (the committed one predated the
220k / 47k AUX sense divider and listed 22k / 33k, which would have put about 7 V on GPIO5). Fab files: `fab/` (`python3 ../tools/fab_export.py AICard`).

**KiCad 10 pass (2026-11-22).** Both files upgraded with `kicad-cli sch upgrade` / `pcb upgrade`; nothing on the board moved.
- **ERC** (KiCad 10.0.2): 0 errors, 409 warnings: 396 endpoints off grid, 13 library notes (the `AICard` symbol nickname is not in a
  library table; J4's and the LEDs' symbols differ from KiCad 10's copies).
- **DRC with the schematic parity check** (`generate/drc.py`, now kicad-cli as on the other cards): 0 errors, 0 unconnected.
  Warnings: 68 footprints differ from KiCad 10's library copies, 68 *Description* fields set in the schematic only, 20 silkscreen past
  the edge (J1, J3, J4, on purpose), 5 silkscreen over copper, 2 silkscreen overlaps. Parity otherwise clean: every part, value and net matches.
- **Mask dams:** `tools/mask_dams.py` narrowed J4's eight 0.5 mm-pitch signal pads to 0.28 mm (0.22 mm gaps); zones refilled.
- **AP7361C-33E pinout checked** against the Diodes datasheet: suffix E is SOT223 (ER is the reversed SOT223R), pin 1 IN, 2 GND
  (tab), 3 OUT, as drawn. ConsoleBoardRS uses the same part and pinout.
- **Power tracks** measured in the routed board (the 5 V pre-route was blocked and the router laid them): MOD_5V 1.0 mm on F.Cu
  and B.Cu (a 0.8 mm stub at a pad), AUX12 and AUX_GND 0.8 mm, BUCK_SW 1.0 mm, the slot +5 V 0.3 mm (it feeds only the ESP32 side).
- **Fab files** re-exported with KiCad 10 (`fab/`, inner layers now `.g1` / `.g2`, plus a JLC CPL).

What has **not** been checked, and what to do about it before ordering:
- **The compute module** (size, header end, mounting holes, which pins it powers from): see the box at the top.
- **The buck** is the main board's circuit with a smaller inductor and the 470 µF at the load; its loop was not simulated. Check L1's
  ratings (Isat ≥ 5 A, Irms ≥ 3 A) and the compensation with TI WEBENCH or a load-step test on the real module.
- **J4 (the GCT USB4105)**: its own locating hole is 0.194 mm from pad B12, so the project's minimum hole clearance is set to
  0.19 mm instead of 0.25 mm; confirm the fab's limit. Only the A4 / B9 VBUS contacts are wired (the others sit 4.8 mm away across the
  signal pads, and VBUS powers nothing here).
- **Thermal**: nothing has been measured; the design relies on the two-step throttle in the firmware (`docs/hardware/ai_card.md`).
- **AUX budget (changed on the main board after this card was drawn).** Since 2026-10-19 DigiDash2's AUX 12 V is sized for
  about **3 A for all slots together** (5 A fuse; it was 10 A). This card's 3 A peak at 5 V is about 1.4 A from 12 V, and the
  LTE card's modem buck about 1 A: both fit, with little margin. Measure the module's peak before adding another AUX user.
- **The module's UART on header pins 8 / 10** may be its Linux debug console (on Radxa Zero 3 boards it usually is, at
  1.5 Mbaud): either turn the console off in the module's image or move the link to another UART. Confirm on the module.
- **Slot numbering was fixed on the main board on 2026-10-19** (`accessory_ports.md`, Numbering). The card side did not change
  and J1 is placed as on the audio and LTE cards, so this card should mate; confirm in KiCad's 3D viewer with DigiDash2.
- **No PCBWay BOM yet** (`fab/pcbway/`, as on the other cards). Every part is SMD except J1–J3, which PCBWay can fit too.
- **Pin 1 of J1** against the main board's slot, and the USB-C and debug header against the case, in KiCad's 3D viewer.

## Regenerating

**The generators still write KiCad 7 format** (`gen_sch.py`, `gen_pcb.py`): after a regeneration run `kicad-cli sch upgrade` and
`kicad-cli pcb upgrade` on the two files, then `tools/mask_dams.py AICard` and `gen_pcb.py --fill`. `--fill` runs on KiCad 10's
Python (`$KIPY`); the full place-and-route has only been run on KiCad 7.

KiCad's stock libraries and `kicad-cli`, a Python with `pcbnew` and `numpy`, and Freerouting (Java; **2.4.1 needs Java 25** and
runs with `--gui.enabled=false`; 1.9.0 with Java 17+ also worked for the LTE card). This card was generated with KiCad
**7.0.11** on Linux (`apt install kicad python3-numpy`, Python 3.12); KiCad 7's Python API has no net-class assignment and no
connectivity walk, so the script writes the net classes into the `.kicad_pro`, splits them into the DSN for the router, reads the
router's session itself (`import_ses`) and uses `pcbnew.WriteDRCReport` for DRC. On KiCad 8+ the same script runs unchanged.
Temp files go to `$TMPDIR` with `aicard` in their names.

```bash
python3 generate/gen_sch.py                  # also writes generate/refs.json (part groups for placement)
python3 generate/gen_bom.py
JAVA=/usr/lib/jvm/java-25-openjdk-amd64/bin/java FR_PASSES=60 \
  /usr/bin/python3.12 generate/gen_pcb.py --route freerouting-2.4.1-executable.jar     # place, route, stitch, fill
/usr/bin/python3.12 generate/drc.py
```

`SKIP_ROUTE=1` reuses the last session instead of running the router; `--stitch` / `--fill` re-run only the ground stitching /
zone fill on the routed board. Hand fixes after the routing (if any) are listed under Status and are lost on a re-run.
