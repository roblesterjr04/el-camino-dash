# Console firmware (ESP32-S3, CAN node 0x13)

Firmware for the RetroSound console board ([`ConsoleBoardRS/`](../../ConsoleBoardRS/)): the SSD1322 256 × 64 OLED
and two **RetroSound knobs** (left = Volume, right = Select) on RJ9 jacks, its only controls. It is the car's radio
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
| Push | mute (radio, aux in) / play-pause (Bluetooth); audio on when it is off; the microphone in a call | **OK**: at rest the menu; in a list pick the row; in an editor done; in the preset list tune to the one highlighted. A ringing call: answer; in a call: its audio to the phone / back to the car |
| Hold | the next source, round Bluetooth → Radio (when heard) → Aux in (when the audio card has it) → Off → Bluetooth | **the voice assistant** (radio or Bluetooth); in the menu: leave it |
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
| Equalizer | Back (hold: Exit) | Presets (the preset list) |
| Editor | Undo | Done |
| Confirmation | No | Yes |
| Incoming call | Decline | Answer |
| Call waiting | Ignore | Answer (holds the active call) |
| In a call | End | Mute / Unmute |
| Call on hold | End | Swap |
| Dialing | End | |
| Pairing code | Reject | Pair |

In reverse both keys are blank and only the volume works. Bluetooth commands go to the audio card as AUDIO_CMD MEDIA /
VOLUME; a separate audio bridge (one at another node than the audio sink) gets the old phone commands (`0x215`) as
well, but the audio card, which is both, gets each command once. Radio commands go to the audio card, which forwards
them to the radio (`docs/protocols/can_channels.md`, AUDIO_CMD). With aux in playing the right knob and the left ring
do nothing. The rest lives in the menu.

**The preset list** (Presets, the right ring left, with the radio playing): the twelve presets, four by three, with their
frequencies; the one playing is outlined. Turning moves the highlight only; the push tunes to the preset highlighted
and closes the list (AUDIO_CMD MEDIA 13, recall). Close shuts it without changing the station; left alone for 4 s it
closes by itself.

**The menu** (right knob push at rest) is drawn on the OLED itself; the touch screens' menu is not mirrored here.

**The Equalizer** (Sound → Equalizer): ten bars, 31 Hz to 16 kHz, on a 0 dB line, -12 to +12 dB. The right knob moves
along the bands; its push edits the one highlighted, and turning then changes it live (each detent a UI SET, so it is
heard as it turns); push again or Done keeps it, Undo puts it back. Presets (the right ring right) opens the preset
list: Flat, Bass, Treble, Vocal, Rock, Jazz, Custom. Moving a band on a preset makes it Custom (the audio card keeps the
Custom curve while another preset plays). The values are the audio card's own items
(`docs/protocols/can_channels.md`, *The sink's page*).

| Page | What it holds |
| --- | --- |
| Source | Bluetooth, Radio (when a radio card is heard), Aux in (when the audio card has one), Off (no source: Bluetooth paused, the radio muted; chimes still sound) |
| Radio | the radio card's own page from the UI channel: on / off, band, frequency, seek, presets, region… |
| Phone | the connected phone, Pair new phone / Stop pairing, voice assistant, redial, the paired phones (Connect or Disconnect, Forget with a confirmation) |
| Sound | the audio card's page (its preset shows on the row): chime volume, fader, balance, and the **Equalizer** |
| Accessories | every accessory's page from the UI channel (the audio card, GPS, the gateway…) |
| Display | center view, theme, brightness, dim with lights, menu screen, media screen, right layout, OLED contrast (live, undone with Undo) |
| Trip | Trip A, and resetting it |
| Service | the center, the sensors, the audio bridge, the audio card and its source, the radio, the channel nodes |

## The knobs

Each RetroSound knob has two analog lines on its RJ9 jack (33k pull-ups, ADC1;
[`ConsoleBoardRS/README.md`](../../ConsoleBoardRS/README.md#knobs-rj9)): the encoder (open → 82k → 30k → 47k →
open, detents at open and 30k, two steps per click) and the switches (open, 6.8k ring left, 22k ring right, 0 Ω
push). A task on core 0 reads all four lines every millisecond with `analogReadMilliVolts` and keeps a line's level
once it has read the same band three times running, so the RC's settling and contact bounce never count. Bands:
push < 0.28 V < ring left < 0.94 V < ring right < 1.445 V < 30k < 1.755 V < 47k < 2.15 V < 82k < 2.65 V < open.

Only the encoder reads 1.445-2.65 V and only the switches below 1.445 V, so the firmware tells the lines apart: when
one shows up in the other's range it swaps the pair (a reversed phone cord) before acting on it, and the first touch
of the knob after power-up settles it. `knobs` on the serial console shows each line's voltage, band and role: check
the bands against real knobs on the bench, and set `CONSOLE_SEL_REVERSE` / `CONSOLE_VOL_REVERSE` if one turns the
wrong way.

## The IR remote

An optional 38 kHz IR receiver on J6 (a TSOP38438 or TSOP4838: OUT to GPIO 39 (`CONSOLE_IR_PIN`, -1 = none), GND,
VS on **3.3 V**) takes two NEC remotes (`src/ir_remote.cpp`): the classic Apple remote, white or aluminum (Apple's
0x87EE, any pairing ID), and the old Sirius radio remote (address 0x01). Their buttons do what the knobs do:

| Button (Apple / Sirius) | Click | Hold |
| --- | --- | --- |
| Up / Down (▲ / ▼) | the right knob's turn: up the list in the menu and the preset list; at rest and in an editor Up is clockwise (tune up, next track, more). Held: repeats | |
| Select | the right knob's push (OK) | the right knob's hold (voice assistant; in the menu, leave it) |
| Left / Right (CATEGORY ◄ / ►) | the left / right soft key | the soft key's hold |
| Menu / MENU | at rest the menu; in it the left soft key (Back, Undo, No, Exit, Close) | leave the menu |
| Sirius + / − | the left knob's turn (volume). Held: repeats | |
| Sirius SEL | in the menu, an editor, the preset list or a call: the right knob's push (OK); otherwise the left knob's push (play / pause, mute) | in the menu: leave it; otherwise the next source |
| Play (aluminum) / Sirius MUTE | the left knob's push (play / pause, mute) | |
| Sirius BAND | the left knob's hold (the next source) | |
| Sirius power | the audio off (as the Source page's Off); off already: the source it had back | |
| Sirius DISPLAY | the next drive page (Media, Trip, Engine, Clock) | the next center-screen view |
| Sirius 1-9 | recall that radio preset, on any screen (an empty slot, or the radio not playing, only says so) | |

The Sirius remote's MEMO, PREV and 0 do nothing yet. A remote never says a button is up: it is
let go 200 ms after its last repeat, so clicks land then. The DevKitC's RGB LED (`CONSOLE_IR_LED_PIN`, 48; free on the
console board) flashes blue for any IR and green for a known remote's button. On the serial console `ir` shows the
last code heard and the button down, `ir log on` prints every burst's marks and spaces (us) and what it decoded to,
and `remote KEY [hold]` presses a button without a remote.

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
