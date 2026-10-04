# Console firmware (ESP32-S3, CAN node 0x13)

Firmware for the RetroSound console board ([`ConsoleBoardRS/`](../../ConsoleBoardRS/)): the SSD1322 256 × 64 OLED
and two **RetroSound knobs** (left = Volume, right = Select) on RJ11 jacks, its only controls. It is the car's radio
head unit: sources, the radio, volume, the phone (calls, pairing, the paired phones) and every accessory's settings,
with no touch screen needed. Spec: [`docs/protocols/can_bus.md`](../../docs/protocols/can_bus.md), *Phone* and
*Console*.

```bash
./flash.sh --upload-port /dev/cu.usbmodem…      # build + flash over the board's USB-C
test/host/run_tests.sh                           # UI state machine tests on the Mac (no toolchain needed)
```

| File | |
| --- | --- |
| `src/console_ui.{h,cpp}` | The state machine: CAN frames and knob input in, a screen model, the soft keys and CAN commands out. No Arduino code |
| `src/console_screens.cpp` | Draws every screen into a 16-grey frame (`oled_gfx`), with the soft keys' labels in the bottom corners |
| `src/console_input.cpp` | The knobs: a 1 kHz ADC task (below), click on release, hold at 650 ms |
| `src/console_pins.h` | Pins and input build flags |
| `src/main.cpp` | CAN (`elc_can`), the loop and the serial console |

## Controls

Each knob has a turn (in clicks), a push and an outer ring that pushes left or right and springs back. Three rules:

- **The left knob is the audio, on every screen** (the menu too): it never moves a selection.
- **The right knob moves through what is on the screen**: it tunes or skips at rest, moves the highlight in a list or
  changes a value. Its push is **OK** (at rest: the **menu**), its hold the **voice assistant**.
- **The right ring is two soft keys.** Their labels sit in the screen's bottom corners and change with what is on the
  screen: push the ring toward a label to use it. The **right one is yes / the main action / forward** (Answer, Pair,
  Done, Yes, OK, the next page), the **left one is no / back / the second action** (Decline, Reject, Undo, No, Back,
  Presets, Redial). The labels light up for a couple of seconds after any control is touched, and stay lit while something
  waits for an answer (a call, a pairing code, a warning).

Every push and the right ring have a click (on release) and a hold (`CONSOLE_HOLD_MS`, 650 ms); a soft key with no
hold of its own does its click when held. The left ring acts once per flick.

### The knobs

| | Left knob (Volume) | Right knob (Select) |
| --- | --- | --- |
| Turn | volume (the call's volume in a call) | at rest: tune one grid step (radio) / next or previous track (Bluetooth). In a list: the highlight. In an editor: the value |
| Push | mute (radio) / play-pause (Bluetooth); audio on when it is off; the microphone in a call | **OK**: at rest the menu; in a list pick the row; in an editor done; in the preset list tune to the one highlighted. A ringing call: answer; in a call: its audio to the phone / back to the car |
| Hold | the next source, round Bluetooth → Radio (when heard) → Off → Bluetooth | **the voice assistant** (radio or Bluetooth); in the menu: leave it |
| Ring left / right | seek down / up (radio), previous / next track (Bluetooth), on every screen | the soft keys, below |

### The soft keys (the right ring)

| Screen | Left | Right |
| --- | --- | --- |
| Drive page, radio | Presets (the preset list; hold: store in the next free slot) | the next page's name: Trip → Engine → Clock → Radio / Media (hold: next center-screen view) |
| Drive page, a phone connected for calls | Redial (play / pause is the left knob's push) | next page |
| Drive page, no phone for calls | Phone (the phone page: pair, connect) | next page |
| Drive page, audio off | Audio on | next page |
| Trip page | Reset (hold; a click says so) | next page |
| Preset list | Close (without changing the station) | Store (hold: into the slot highlighted) |
| Warning | | OK (acknowledge) |
| Menu, top page | Exit | |
| Menu, deeper | Back (hold: Exit) | Exit |
| Editor | Undo | Done |
| Confirmation | No | Yes |
| Incoming call | Decline | Answer |
| Call waiting | Ignore | Answer (holds the active call) |
| In a call | End | Mute / Unmute |
| Call on hold | End | Swap |
| Dialing | End | |
| Pairing code | Reject | Pair |

In reverse both keys are blank and only the volume works. Bluetooth commands go to the audio bridge (`0x215`, as
before) and, when an audio card is heard, to it too (AUDIO_CMD MEDIA / VOLUME); radio commands go to the audio card,
which forwards them to the radio (`docs/protocols/can_channels.md`, AUDIO_CMD). The rest lives in the menu.

**The preset list** (Presets, the right ring left, with the radio playing): the twelve presets, four by three, with their
frequencies; the one playing is outlined. Turning moves the highlight only; the push tunes to the preset highlighted
and closes the list (AUDIO_CMD MEDIA 13, recall). Close shuts it without changing the station; left alone for 4 s it
closes by itself.

**The menu** (right knob push at rest) is drawn on the OLED itself; the touch screens' menu is not mirrored here.

| Page | What it holds |
| --- | --- |
| Source | Bluetooth, Radio (when a radio card is heard), Off (no source: Bluetooth paused, the radio muted; chimes still sound) |
| Radio | the radio card's own page from the UI channel: on / off, band, frequency, seek, presets, region… |
| Phone | the connected phone, Pair new phone / Stop pairing, voice assistant, redial, the paired phones (Connect or Disconnect, Forget with a confirmation) |
| Accessories | every accessory's page from the UI channel (the audio card, GPS, the gateway…) |
| Display | center view, theme, brightness, dim with lights, menu screen, media screen, right layout, OLED contrast (live, undone with Undo) |
| Trip | Trip A, and resetting it |
| Service | the center, the sensors, the audio bridge, the audio card and its source, the radio, the channel nodes |

## The knobs

Each RetroSound knob has two analog lines on its RJ11 jack (33k pull-ups, ADC1;
[`ConsoleBoardRS/README.md`](../../ConsoleBoardRS/README.md#knobs-rj11)): the encoder (open → 82k → 30k → 47k →
open, detents at open and 30k, two steps per click) and the switches (open, 6.8k ring left, 22k ring right, 0 Ω
push). A task on core 0 reads all four lines every millisecond with `analogReadMilliVolts` and keeps a line's level
once it has read the same band three times running, so the RC's settling and contact bounce never count. Bands:
push < 0.28 V < ring left < 0.94 V < ring right < 1.445 V < 30k < 1.755 V < 47k < 2.15 V < 82k < 2.65 V < open.

Only the encoder reads 1.445-2.65 V and only the switches below 1.445 V, so the firmware tells the lines apart: when
one shows up in the other's range it swaps the pair (a reversed phone cord) before acting on it, and the first touch
of the knob after power-up settles it. `knobs` on the serial console shows each line's voltage, band and role: check
the bands against real knobs on the bench, and set `CONSOLE_SEL_REVERSE` / `CONSOLE_VOL_REVERSE` if one turns the
wrong way.

## Build flags

| Flag | Default | |
| --- | --- | --- |
| `CONSOLE_SEL_REVERSE` / `CONSOLE_VOL_REVERSE` | 0 | 1 = the knob counts the wrong way |
| `CONSOLE_KNOB_STABLE_SAMPLES` | 3 | 1 ms samples a knob line must hold its band to count |
| `CONSOLE_HOLD_MS` | 650 | a push or the right ring held this long is its hold action |
| `ELC_CAN_TX_PIN` / `ELC_CAN_RX_PIN` | 5 / 6 | TWAI to the MCP2562 (`-1` = CAN off) |

## Serial console (USB, 115200)

For the bench, without knobs wired: `help`, `state` (CAN, phone, the audio card's source and what plays, cluster),
`chan` (the channel node's announced nodes and accessory pages), `keys` (what the two soft keys say now),
`key left|right [hold]` (the right ring), `left +N|-N|click|hold|ringl|ringr`, `right +N|-N|click|hold`, `knobs` (the
knob lines), `demo on` (a pretend center, sensors and phone). Every
command the console sends is logged as `tx 0x215 phone ANSWER arg 0 slot 0 list 3`.

The console follows the first audio bridge it hears: a node whose heartbeat says audio bridge (node type 1). Its
node follows its card slot (`0x14` for the audio card in slot 1). Until a bridge is heard, phone commands are not
sent at all, and frames in another card's message range are ignored.
