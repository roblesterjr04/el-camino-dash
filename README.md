# El Camino Dash

A modular, open hardware and firmware digital instrument cluster, built on a CAN bus and plug-in cards.

The reference build is the dash of a **1964 Chevrolet El Camino**: three round touch screens sit where the original
gauges were, a main board behind them is shaped to that dash, and a small console works like the car's radio. But
only the main board knows which car it is in. Everything else (the screens, the sensor card, the audio card, the
radio, GPS, the LTE gateway, the console) is a node on one CAN bus that plugs into a **standard 2×15 card slot** or a
4-pin cable port. **Draw a main board for your own car, boat, kit car or bench, give it the same slots, and every card
in this repository plugs straight in.** Design a new card, and it adds its readings, alerts and settings pages to
the dash without a line of change to the screen firmware.

> **Status: work in progress.** The display, console and audio firmware run on bench hardware (Waveshare ESP32-P4
> round display boards, ESP32 dev kits). Nothing is installed in the car yet. Several cards exist as KiCad designs and
> as simulators in [`tools/`](tools/) but have not been built. Each README states what has and has not been tested
> on hardware. See [Status](#status).
>
> **Updated often; issues are not monitored, pull requests are welcome.** See [Contributing](#contributing).

## Contents

- [A modular platform](#a-modular-platform)
- [What it does](#what-it-does)
- [System overview](#system-overview-the-el-camino-reference-build)
- [Hardware](#hardware)
- [Firmware](#firmware)
- [Repository layout](#repository-layout)
- [Getting started](#getting-started)
- [Tests](#tests)
- [Making the boards](#making-the-boards)
- [Documentation map](#documentation-map)
- [Status](#status)
- [History](#history)
- [Contributing](#contributing)
- [Safety](#safety)
- [License](#license)

## A modular platform

```
          your main board                              the cards (unchanged, from any project)
  ┌───────────────────────────────┐
  │ shaped for your dash          │      ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐
  │ 1–3 screen headers            │◄─────┤ sensor │ │ audio  │ │ radio  │ │  GPS   │ │ yours  │
  │ 1–6 card slots (2×15)         │ slot │ card   │ │ card   │ │ card   │ │ card   │ │        │
  │ power, fuses, WAKE            │      └────────┘ └────────┘ └────────┘ └────────┘ └────────┘
  │ CAN bus, terminated at ends   │
  │ cable ports ──► console       │      every card: CAN + power on the slot, node number from the slot,
  └───────────────────────────────┘      services found by discovery, no configuration
```

The platform is three agreements, each documented in full:

| Agreement | What it fixes | Document |
| --- | --- | --- |
| **The card slot** | A 2×15, 2.54 mm socket: power (5 V, 3.3 V, switched and constant 12 V, AUX 12 V), CAN, a slot-ID resistor, WAKE, dash-light and reverse levels, an I2S digital audio rail and an analog audio bus. 80 × 60 mm cards standing on edge | [`docs/hardware/main_board_guide.md`](docs/hardware/main_board_guide.md) (designing a main board), [`docs/hardware/accessory_ports.md`](docs/hardware/accessory_ports.md) (the slot spec) |
| **The CAN bus** | Classic CAN, 500 kbit/s, terminated only on the main board; node numbers from the slot (`0x14 + slot − 1`); IDs as `channel base + node` | [`docs/protocols/can_guide.md`](docs/protocols/can_guide.md) (the guide), [`docs/protocols/can_bus.md`](docs/protocols/can_bus.md) |
| **The channel protocol** | How a card announces itself and uses the dash: alerts, sensor readings, menu pages, input, sounds, discovery | [`docs/protocols/can_channels.md`](docs/protocols/can_channels.md) |

What that buys you:

- **A main board is cheap to redraw.** It has no microcontroller: just power, the bus, the slots, the screen headers
  and the vehicle connectors. [`BenchBoard/`](BenchBoard/) (100 × 100 mm, two slots, one screen) is a complete
  small example; [`DigiDash2/`](DigiDash2/) is the full El Camino board.
- **Cards need no configuration.** A card reads its slot from a resistor on the main board and takes its node number
  from it. Two identical cards never clash; moving a card just renumbers it.
- **The dash finds services by discovery.** Cards announce what they produce and consume; the screens find the audio
  sink, the GPS or the internet gateway from those announcements.
- **New cards extend the dash without firmware changes elsewhere.** A card that publishes a SENSOR reading gets it
  logged and shown; one that defines an ALERT gets a chime and a banner; one that describes UI items gets a page in
  the menu on the round screens and on the console.
- **Fewer screens or slots is fine.** One screen runs as the center; slots can be any subset of the six.
- **The sensor card is a card too.** If your car's senders, speed sensor or tach differ, change the sensor card
  (or calibrate it from the menu), not the main board.

Ideas for cards nobody has built yet: transmission or oil temperature, wideband O2, tire pressure, parking sensors
(PLAY a tick sound on the audio card), steering-wheel buttons (INPUT), a data logger, a voice assistant on the I2S rail.
`tools/cards_sim.py` already simulates several of them, so the dash side can be tried before the hardware exists.

## What it does

- **Gauges.** Speed, odometer and trip, tach, fuel, oil pressure, coolant temperature, battery voltage, charge lamp,
  efficiency. Vector faces (LVGL) scale to 720×720 and 800×800 panels. If sensor data stops for 400 ms the needles
  fall to rest and the face shows **NO DATA** rather than a false zero.
- **Themes.** Classic (black glass, chrome, cream numerals), Modern (flat, orange arcs) and Fallout (green phosphor).
  A long press on the center screen changes all three screens at once.
- **Sensors on CAN.** The sensor card reads the original senders, the speed sensor, the tach signal, ignition, battery
  and the alternator lamp, keeps the odometer in FRAM, and sends the readings on CAN. Calibration is done from the
  menu ([`docs/protocols/sensor_calibration.md`](docs/protocols/sensor_calibration.md)).
- **Audio and phone.** The audio card is a Bluetooth A2DP / AVRCP / HFP sink with two stereo DACs (front and rear), a
  microphone input, a 10-band equaliser, fader and balance. Now playing, album art and the call card appear on the
  screens; calls are answered from the console or the touch screen.
- **Radio head unit.** The console (ESP32-S3, 256×64 OLED, two RetroSound-style knobs, IR receiver) selects the
  source, tunes the radio, handles calls and pairing, and shows every card's settings page.
- **Accessory protocol.** Cards announce themselves and publish alerts, sensors, menu pages, input and sounds through a
  documented CAN channel protocol ([`docs/protocols/can_channels.md`](docs/protocols/can_channels.md)), so a new card
  can add a page to the menu without changing the screen firmware.
- **More.** GPS time and location with a POSIX time zone, a reversing camera on the center screen (TVP5150 decoder),
  per-screen touch menus, settings kept in NVS and on SD. The cluster never uses Wi-Fi.

## System overview: the El Camino reference build

```
                         ┌──────────────── DigiDash main board (behind the screens) ────────────────┐
  vehicle 12 V ─────────►│ power, ignition / WAKE, reverse and dash-light sense, video decoder     │
  senders, speed, tach ─►│                                                                          │
  camera ───────────────►│  [J5] LEFT screen        [J1] CENTER screen        [J4] RIGHT screen     │
                         │  speedometer             tach / clock / media /    fuel / oil / temp /   │
                         │  ESP32-P4, node 0x02     efficiency, camera, GPS   charge                │
                         │                          ESP32-P4, node 0x01       ESP32-P4, node 0x03   │
                         │                                                                          │
                         │  six card slots (2×15): CAN, 5 V, 3.3 V, AUX 12 V, slot ID, I2S rail     │
                         │   ├ Sensor card   (node 0x04)   senders, speed, tach, odometer (FRAM)    │
                         │   ├ Audio card    (node 0x14+)  Bluetooth, DACs, mic, EQ; I2S clock master│
                         │   ├ Radio card                  Si4735 AM/FM with RDS                    │
                         │   ├ GPS card                    remote GPS puck                          │
                         │   └ LTE card                    SIMCom A7670 gateway                     │
                         │                                                                          │
                         │  cable ports ── ConsoleBoardRS (node 0x13): OLED, 2 knobs, IR            │
                         │  USB-CAN tap                                                             │
                         └──────────────────────────── CAN 500 kbit/s ──────────────────────────────┘
```

- **One CAN bus** (500 kbit/s, 11-bit IDs, terminated at the left and right screens) links everything. The cluster's
  own traffic (sensor frames, theme, settings, media, phone, console) is in
  [`docs/protocols/can_bus.md`](docs/protocols/can_bus.md); the accessory channel protocol that cards use is in
  [`docs/protocols/can_channels.md`](docs/protocols/can_channels.md).
- **The three screens run one firmware image.** Each reads its position from a strap on the main board's header
  (left to GND, right to 3.3 V, center open) and becomes the speedometer, the center screen or the right screen.
- **The center screen** owns the cluster settings, GPS time and location, the time zone, the camera and SD storage,
  and re-sends theme and settings to the others. Any screen can host the touch menu.
- **The sensor card is the source of truth** for readings and the odometer. A serial fallback (the original UART fan-out)
  can still feed the screens directly through a jumper.
- **The I2S rail** is a five-line digital audio bus to every slot. The audio card is its clock master; the radio and
  other cards ask for its return line before they send audio.

Pinouts, connectors and power are in [`docs/hardware/accessory_ports.md`](docs/hardware/accessory_ports.md) and
[`WIRING_PINOUT.md`](WIRING_PINOUT.md).

## Hardware

Every board is a KiCad 10 project generated by Python scripts in its `generate/` folder. The full list, with revision
history and status, is in [`HARDWARE.md`](HARDWARE.md).

| Board | What it is |
| --- | --- |
| [`DigiDash2/`](DigiDash2/) | Main board, rev 2: three screen headers, six card slots, power, video decoder, 4 layers, SMD assembled by the fab |
| [`DigiDash3/`](DigiDash3/) | Main board, rev 3: rev 2 with every part through-hole, 2 layers, for hand assembly |
| [`DigiDash/`](DigiDash/) | Main board, rev 1 (superseded): plug-in dev boards and accessory headers |
| [`SensorCard/`](SensorCard/) | Sensor card rev A: ATmega2560 + MCP2515 |
| [`SensorCard2/`](SensorCard2/) | Sensor card rev B: STM32G0B1 with built-in FDCAN, 2 layers |
| [`AudioCarrier/`](AudioCarrier/) | Audio card: ESP32-DevKitC-VE in sockets, 2× PCM5102A DAC, PCM1808 ADC + mic preamp, RCA outputs |
| [`RadioCard/`](RadioCard/) | AM/FM radio card: Si4735 tuner, STM32G0B1 |
| [`GPSCard/`](GPSCard/), [`GPSCard2/`](GPSCard2/) | GPS card: STM32G0B1 (rev A, SMD) or PIC18F26Q83 (rev B, through-hole) |
| [`LTECard/`](LTECard/) | LTE gateway card: ESP32-S3 + SIMCom A7670 core board |
| [`AICard/`](AICard/) | AI voice card: ESP32-S3 front end + a Pi-header Linux compute module (speech, local and online models). Not ordered; the module's fit is provisional |
| [`ConsoleBoardRS/`](ConsoleBoardRS/) | Console: ESP32-S3, SSD1322 OLED, RetroSound knobs on RJ9, IR receiver |
| [`ConsoleBoard/`](ConsoleBoard/) | Console, earlier variant with keys and encoders on the board |
| [`BenchBoard/`](BenchBoard/) | Bench stand-in for the main board: two slots, one screen, a USB-CAN header |
| [`Panel/`](Panel/) | All current boards on one fab panel |
| [`Housing/`](Housing/) | Onshape FeatureScript for the cluster housing; a turn-signal indicator puck |

Off-the-shelf parts: the screens are **Waveshare ESP32-P4-WIFI6-Touch-LCD-4C** (4", 720×720) on the left and right and
**-3.4C** (3.4", 800×800) in the center; they plug into the main board on 2×20 headers.

## Firmware

PlatformIO projects under [`firmware/`](firmware/). Shared code is in libraries; every board runs the firmware that
ships (there are no separate bench builds). Details, gestures, console commands and panel targets:
[`firmware/README.md`](firmware/README.md).

| Directory | Runs on | What |
| --- | --- | --- |
| [`firmware/esp32_p4_display`](firmware/esp32_p4_display/) | All three screens (ESP32-P4) | One image; position from the strap |
| [`firmware/esp32_p4_ui`](firmware/esp32_p4_ui/) | library | LVGL screen layer, gauge faces, menu, media / phone / accessory pages, touch |
| [`firmware/esp32_p4_common`](firmware/esp32_p4_common/) | library | Sensor link, panel setup, cluster I2C, watchdog, boot splash |
| [`firmware/elc_can`](firmware/elc_can/) | library | CAN IDs, frame codecs, TWAI driver with heartbeat and bus-off recovery |
| [`firmware/esp32s3_console`](firmware/esp32s3_console/) | Console (ESP32-S3) | Radio head unit UI on the OLED |
| [`firmware/esp32_classic_bt_audio`](firmware/esp32_classic_bt_audio/) | Audio card (classic ESP32) | Audio sink and Bluetooth bridge; builds [`arduino/sketches/esp32_audio_bridge`](arduino/sketches/) |
| [`firmware/sensor_card`](firmware/sensor_card/) | Sensor card rev A (ATmega2560) | Senders, speed, tach, odometer, calibration |
| [`firmware/sensor_card_stm32`](firmware/sensor_card_stm32/) | Sensor card rev B (STM32G0B1) | Port of rev A; same logic, new hardware layer |
| [`firmware/ai_card`](firmware/ai_card/) | AI voice card (ESP32-S3) | CAN node, rail audio, wake word, compute-module power, the assistant's allow-list |
| [`firmware/ai_agent`](firmware/ai_agent/) | AI card's compute module (Linux) | Speech to text, intents, on-board and online models, speech |
| [`firmware/esp32_p4_display_litmus`](firmware/esp32_p4_display_litmus/) | Any P4 panel | Panel bring-up test |
| [`arduino/sketches/`](arduino/sketches/) | various | Rev 1 sensor pack (ATmega328P), the audio bridge source, older experiments |

The radio, GPS, LTE and other cards do not have firmware yet; their behaviour on CAN is simulated by
[`tools/can_relay.py`](tools/can_relay.py) and the `*_sim.py` modules next to it.

## Repository layout

```
el-camino-dash/
├── README.md               this file
├── HARDWARE.md             every board: lineage, status, how to regenerate and order
├── WIRING_PINOUT.md        vehicle harness and screen header pinouts
├── firmware/               PlatformIO firmware and shared libraries
├── arduino/                Arduino sketches and arduino-cli helpers (rev 1 sensor pack, audio bridge source)
├── docs/
│   ├── protocols/          wire formats: CAN bus, CAN channels, sensor UART, USB link, calibration, I2C
│   └── hardware/           specs and plans: accessory ports, cards, housing envelope, harnesses, costing
├── tools/                  bench relay and card simulators, gauge / console previews, fab export scripts
├── scripts/                asset export (boot splash to RGB565)
├── assets/                 boot splash and the original Classic gauge art (design reference)
├── <Board>/                one directory per KiCad board (see Hardware)
└── Housing/                enclosure CAD
```

Each directory has its own README.

## Getting started

### Prerequisites

- **Python 3** for the tools and generators.
- **PlatformIO 6.1.18 or later.** Each `flash.sh` installs it into a local `.venv`; for a PlatformIO you installed
  yourself, add the builder's Python packages with `firmware/requirements-pio.txt` (see
  [`firmware/boards/README.md`](firmware/boards/README.md)). Platform and library versions are pinned in each `platformio.ini`.
- **A C++17 compiler** (Apple clang or gcc) for the host tests and the previews.
- **KiCad 10** to open or regenerate the boards.

### See the gauges without hardware

The faces render on a desktop with the same code that runs on the screens:

```bash
cd firmware/esp32_p4_display && pio run -e display-waveshare3c4 && cd -   # once, to fetch LVGL
tools/gauge_preview/build.sh
python3 tools/gauge_preview/preview.py faces.png 0,1,0 2,1,0 1,1,0
```

The console's OLED has the same kind of preview in [`tools/console_preview/`](tools/console_preview/). See
[`tools/README.md`](tools/README.md).

### Flash a screen

Any Waveshare ESP32-P4 round display board will do. Pick the environment that matches the panel
(`display-waveshare3c4` = 3.4" 800×800, `display-waveshare4c` = 4" 720×720); never flash one panel's profile on the
other.

```bash
cd firmware/esp32_p4_display
./flash.sh -e display-waveshare3c4 --upload-port /dev/cu.usbmodem…
```

Off the main board the position strap is open, so the board starts as the center screen. Type `role left`,
`role right` or `role auto` on its serial console (115200) to change that.

### Run a bench without a main board

[`tools/can_relay.py`](tools/can_relay.py) connects every board plugged into the computer over USB as if they were
on one CAN bus ([`docs/protocols/usb_link.md`](docs/protocols/usb_link.md)). It also sends a simulated drive in place
of the sensor card, the computer's clock in place of GPS, and simulated radio, audio, GPS, LTE and other cards:

```bash
python3 tools/can_relay.py            # every USB serial port found
python3 tools/can_relay.py --frames   # also log the frames
```

With a real CAN bus, plug a USB-CAN adapter into the main board's tap or the [`BenchBoard`](BenchBoard/).

### Other boards

```bash
firmware/esp32s3_console/flash.sh --upload-port /dev/cu.usbmodem…         # console
firmware/esp32_classic_bt_audio/flash.sh --upload-port /dev/cu.usbmodem…  # audio card (DevKitC-VE)
cd firmware/sensor_card_stm32 && pio run -e sensorcard2 -t upload          # sensor card rev B (ST-Link on SWD)
cd firmware/sensor_card && pio run -e sensorcard_isp -t upload --upload-port usb   # sensor card rev A (USBasp on the ISP header)
```

Each firmware README has the details (programmers, environments, console commands).

## Tests

The logic that does not touch hardware is tested on the host with sanitizers. No toolchain beyond a C++ compiler is needed:

```bash
firmware/elc_can/test/host/run_tests.sh               # CAN frame codecs
firmware/esp32_p4_common/test/host/run_tests.sh       # sensor UART parser (currently fails to compile: missing elc_can include path)
firmware/esp32s3_console/test/host/run_tests.sh       # console UI state machine
firmware/esp32_classic_bt_audio/test/host/run_tests.sh  # chimes, DSP, audio sink
firmware/sensor_card/test/host/run_tests.sh           # sensor card rev A logic
firmware/sensor_card_stm32/test/host/run_tests.sh     # sensor card rev B (rev A suite plus its own)
```

Run them all before sending a change that touches shared code.

## Making the boards

- Boards are generated, not hand-drawn: `generate/gen_sch.py`, `gen_pcb.py` and friends write the KiCad files, and a
  `drc.py` checks them. Small changes may be made by hand in the routed `.kicad_pcb` instead; each board README says how.
- `fab/` in each board holds Gerbers, drill files, placement and the assembler's BOM, made by
  `python3 tools/fab_export.py <Board>`. Run `tools/mask_dams.py` after regenerating a board.
- Fabrication and cost notes: [`docs/hardware/panel_order_costing.md`](docs/hardware/panel_order_costing.md) and
  [`HARDWARE.md`](HARDWARE.md).

## Documentation map

Start with [`docs/README.md`](docs/README.md). Building your own main board or card? Read the first two rows. The most important specs:

| Document | Covers |
| --- | --- |
| [`docs/hardware/main_board_guide.md`](docs/hardware/main_board_guide.md) | **Designing your own main board**: the slot contract, CAN wiring, slot IDs, power, screens, bring-up |
| [`docs/protocols/can_guide.md`](docs/protocols/can_guide.md) | **The CAN bus explained**: pins to bus, node numbers, ID map, framing, discovery, every channel, a card's life, worked frames |
| [`docs/protocols/can_bus.md`](docs/protocols/can_bus.md) | Physical layer, node numbers, the cluster's messages (sensors, settings, media, phone, console) |
| [`docs/protocols/can_channels.md`](docs/protocols/can_channels.md) | The accessory channel protocol: NET, SYSTEM, ALERT, SENSOR, UI, INPUT, AUDIO, BULK, ONLINE, LOG |
| [`docs/protocols/usb_link.md`](docs/protocols/usb_link.md) | CAN over USB for the bench |
| [`docs/protocols/sensor_uart.md`](docs/protocols/sensor_uart.md) | The serial sensor frames (rev 1 and the fallback) |
| [`docs/protocols/sensor_calibration.md`](docs/protocols/sensor_calibration.md) | How each reading is calibrated |
| [`docs/hardware/accessory_ports.md`](docs/hardware/accessory_ports.md) | Card slot pinout, I2S rail, power, node numbers |
| [`docs/hardware/digidash_rev2_plan.md`](docs/hardware/digidash_rev2_plan.md) | Why rev 2 looks the way it does |
| [`WIRING_PINOUT.md`](WIRING_PINOUT.md) | Vehicle harness |

## Status

Bench testing only; nothing is in the car. In short:

- **Screens:** run on Waveshare P4 boards on the bench: all three positions, themes, menu, media, phone, call card,
  accessory pages, the camera (capture still stalls about once a second on the bench).
- **Console:** firmware runs on bench hardware; UI state machine host-tested.
- **Audio card:** Bluetooth bridge runs on a DevKitC; the I2S rail is untested on hardware.
- **Sensor card:** rev A and rev B firmware build and are host-tested; neither has run on a card.
- **Radio, GPS, LTE cards:** hardware designed; no firmware yet (simulated in `tools/`).
- **Main board:** rev 2 is routed and DRC clean; rev 3 (through-hole) is in progress.

Open items and known issues are listed in each README under *Status*, *Known issues* or *Open items*.

## History

- The first version ran on a Raspberry Pi with pygame. It is shelved and kept at git tag `pi-pygame-final`.
- The ESP32-P4 rev 1 main board ([`DigiDash/`](DigiDash/)) used an ATmega328P sensor pack on a UART fan-out
  ([`arduino/sketches/sensor_pack`](arduino/sketches/)) and plug-in dev boards.
- Rev 2 ([`DigiDash2/`](DigiDash2/)) moved the sensor pack and all accessories onto slot cards on CAN; rev 3
  ([`DigiDash3/`](DigiDash3/)) is the same board in through-hole parts.

## Contributing

**This repository is updated often, but issues are not monitored.** Questions, bug reports and feature requests
opened as issues will probably go unanswered. **Pull requests are welcome** and are read: a fix, a correction to the
docs, a new card, or a main board for another vehicle. If you find a bug, the best report is a pull request that fixes
it, or one that adds a failing test or a note to the right README's *Known issues*.

- **New card:** claim a product number by adding a row to the
  [products registry](docs/protocols/can_channels.md#products) in your pull request (or use `0x8000` and above for a
  private product that never needs one).
- **New main board:** add it to the repository in its own directory, or send a pull request that links to it from
  here.

Expect the main branch to move; rebase before you open a pull request. Some ground rules that keep the project
consistent:

- **Specs first.** Wire formats live in `docs/protocols/`. Change the spec and the code together, and keep IDs in
  `firmware/elc_can/include/elc_can_ids.h` in step with `can_bus.md` and `can_channels.md`.
- **One firmware per board.** No bench-only builds: bench setups use the USB CAN link and the serial consoles.
- **Run the host tests** for any library you touch.
- **Boards are generated.** Change the `generate/` scripts, regenerate, run the board's `drc.py`, and re-export `fab/`;
  say in the PR whether the board was regenerated or hand-edited.
- **Say what was tested on hardware** and what was not.

## Safety

This is a hobby project, not a certified instrument. A display can freeze, reboot or show a wrong value. Keep a way to
see speed and critical warnings that does not depend on this cluster, check every reading against a known-good gauge
before trusting it, and fuse every connection to the vehicle's 12 V. You use the designs and firmware at your own risk.

## License

Two licenses, split by what the file is:

| What | License | File |
| --- | --- | --- |
| Hardware: every board directory (KiCad schematics, PCBs, footprints, `generate/` output, `fab/`, BOMs), `Housing/` and the mechanical files | CERN Open Hardware Licence v2, Permissive (CERN-OHL-P-2.0) | [`LICENSE-HARDWARE`](LICENSE-HARDWARE) |
| Software and documentation: `firmware/`, `arduino/`, `tools/`, `scripts/`, the board generator scripts, `docs/` and the READMEs | MIT | [`LICENSE`](LICENSE) |

Third-party material keeps its own license: the LVGL fonts in `firmware/esp32_p4_ui/src/fonts/` are generated from Barlow, Saira and VT323 (SIL Open
Font License 1.1) and Material Icons (Apache 2.0); see [their README](firmware/esp32_p4_ui/src/fonts/README.md).
`firmware/esp32_p4_ui/include/FreeSansBold*.h` are Adafruit GFX conversions of GNU FreeFont (GPL with font exception).
The Waveshare panel init tables in `firmware/esp32_p4_common/src/` carry their own SPDX headers (MIT and Apache 2.0).
The BDF fonts in `tools/oled_sim/fonts/` keep the notices in their headers.
