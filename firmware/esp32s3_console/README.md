# Console firmware (ESP32-S3, CAN node 0x13)

Firmware for the 4-key console board ([`ConsoleBoard4Key/`](../../ConsoleBoard4Key/)): the SSD1322 256 × 64 OLED,
**four lit soft keys** in a column left of it, two **RetroSound knobs** (left = Volume, right = Select) on RJ11 jacks,
and the key LEDs. It is the car's radio head unit: sources, the radio, volume, the phone (calls, pairing, the paired
phones) and every accessory's settings, with no touch screen needed. Spec:
[`docs/protocols/can_bus.md`](../../docs/protocols/can_bus.md), *Phone* and *Console*.

```bash
./flash.sh --upload-port /dev/cu.usbmodem…      # build + flash over the board's USB-C
test/host/run_tests.sh                           # UI state machine tests on the Mac (no toolchain needed)
```

| File | |
| --- | --- |
| `src/console_ui.{h,cpp}` | The state machine: CAN frames and key / knob input in, a screen model, the soft keys and CAN commands out. No Arduino code |
| `src/console_screens.cpp` | Draws every screen into a 16-grey frame (`oled_gfx`), with the soft keys' labels down the left edge |
| `src/console_input.cpp` | The keys (20 ms debounce, click on release, hold at 650 ms) and the knobs (a 1 kHz ADC task, below) |
| `src/console_pins.h` | Pins and input build flags |
| `src/main.cpp` | CAN (`elc_can`), the loop and the serial console |

## Controls

Three rules, so nothing has to be remembered per screen:

- **The left knob is the audio, on every screen** (the menu too): it never moves a selection.
- **The right knob moves through what is on the screen**: it tunes or skips at rest, moves the highlight in a list or
  changes a value; its push is **OK**.
- **The keys do what their labels say.** The labels sit in a column down the screen's left edge, top key = top label,
  and change with what is on the screen. The **top key is yes / the main action** (Answer, Pair, Done, Yes, OK,
  Presets, Play), the **bottom key is no / back / the menu** (Decline, Reject, Undo, No, Back, Menu). A key with
  nothing to do shows a dot. The labels light up for a couple of seconds after any control is touched, and stay lit
  while something waits for an answer (a call, a pairing code, a warning).

Every push and key has a click (on release) and a hold (`CONSOLE_HOLD_MS`, 650 ms). The rings are spring-return:
they act once per flick.

### The knobs

| | Left knob (Volume) | Right knob (Select) |
| --- | --- | --- |
| Turn | volume (the call's volume in a call) | at rest: tune one grid step (radio) / next or previous track (Bluetooth). In a list: the highlight. In an editor: the value |
| Push | mute (radio) / play-pause (Bluetooth); audio on when it is off; the microphone in a call | **OK**: at rest the menu; in a list pick the row; in an editor done; in the preset list tune to the one highlighted; answer a ringing call |
| Hold | audio off / back on to the source it had (a radio's power button) | the voice assistant (any source); in the menu, leave it |
| Ring left / right | previous / next source (Bluetooth, Radio) | at rest: seek down / up (radio), previous / next track (Bluetooth). In a list: **Back** / **OK**. In an editor: Undo / Done |

### The soft keys

| Screen | Key 1 (top) | Key 2 | Key 3 | Key 4 (bottom) |
| --- | --- | --- | --- | --- |
| Drive page, radio | Presets (hold: store in the next free slot) | Band (FM ↔ AM) | the next page's name: Trip → Engine → Clock → Radio / Media (hold: next center-screen view) | Menu |
| Drive page, Bluetooth playing | Pause / Play (says what it will do) | Voice (hold: redial) | next page | Menu |
| Drive page, no phone | | Phone (the phone page: pair, connect) | next page | Menu |
| Drive page, audio off | On | | next page | Menu |
| Trip page | as above | Reset (hold; a click says so) | next page | Menu |
| Preset list | Store (hold: into the slot highlighted) | | | Close (without changing the station) |
| Warning | OK (acknowledge) | | | Menu |
| Menu, top page | | | | Exit |
| Menu, deeper | | | Exit | Back (hold: Exit) |
| Editor | Done | | | Undo |
| Confirmation | Yes | | | No |
| Incoming call | Answer | | | Decline |
| Call waiting | Answer (holds the active call) | | | Ignore |
| In a call | Mute / Unmute | Private (the call's audio to the phone) / Car | | End |
| Call on hold | Swap | Mute / Unmute | | End |
| Dialing | | | | End |
| Pairing code | Pair | | | Reject |

In reverse every key is blank and only the volume works. Bluetooth commands go to the audio bridge (`0x215`, as
before) and, when an audio card is heard, to it too (AUDIO_CMD MEDIA / VOLUME); radio commands go to the audio card,
which forwards them to the radio (`docs/protocols/can_channels.md`, AUDIO_CMD). Band sets the radio's Band item
(item 1, `docs/hardware/radio_card.md`).

**The preset list** (Presets): the twelve presets, four by three, with their frequencies (AM in kHz with an "A"); the
one playing is outlined. The right knob moves the highlight only; its push (or the ring right) tunes to the preset
highlighted and closes the list (AUDIO_CMD MEDIA 13, recall). Close or the ring left close it without changing the
station; left alone for 4 s it closes by itself.

**The menu** (right knob push, or Menu) is drawn on the OLED itself; the touch screens' menu is not mirrored here.

| Page | What it holds |
| --- | --- |
| Source | Bluetooth, Radio (when a radio card is heard), Off (no source: Bluetooth paused, the radio muted; chimes still sound) |
| Radio | the radio card's own page from the UI channel: on / off, band, frequency, seek, presets, region… |
| Phone | the connected phone, Pair new phone / Stop pairing, voice assistant, redial, the paired phones (Connect or Disconnect, Forget with a confirmation) |
| Accessories | every accessory's page from the UI channel (the audio card, GPS, the gateway…) |
| Display | center view, theme, brightness, dim with lights, menu screen, media screen, right layout, OLED contrast (live, undone with Undo) |
| Trip | Trip A, and resetting it |
| Service | the center, the sensors, the audio bridge, the audio card and its source, the radio, the channel nodes |

The key LEDs follow the cluster brightness and pulse while a call rings or a code waits.

## The knobs

Each RetroSound knob has two analog lines on its RJ11 jack (33k pull-ups, ADC1;
[`ConsoleBoard4Key/README.md`](../../ConsoleBoard4Key/README.md#knobs-rj11)): the encoder (open → 82k → 30k → 47k →
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
| `CONSOLE_HOLD_MS` | 650 | a push or key held this long is its hold action |
| `ELC_CAN_TX_PIN` / `ELC_CAN_RX_PIN` | 5 / 6 | TWAI to the MCP2562 (`-1` = CAN off) |

## Serial console (USB, 115200)

For the bench, without knobs wired: `help`, `state` (CAN, phone, the audio card's source and what plays, cluster),
`chan` (the channel node's announced nodes and accessory pages), `keys` (what the four keys say now),
`key 1..4 [hold]`, `left +N|-N|click|hold|ringl|ringr`, `right +N|-N|click|hold|ringl|ringr`, `knobs` (the knob
lines). The top key held at power-up starts the demo (a pretend center, sensors and phone). Every
command the console sends is logged as `tx 0x215 phone ANSWER arg 0 slot 0 list 3`.

The console follows the first audio bridge it hears: a node whose heartbeat says audio bridge (node type 1). Its
node follows its card slot (`0x14` for the audio card in slot 1). Until a bridge is heard, phone commands are not
sent at all, and frames in another card's message range are ignored.
