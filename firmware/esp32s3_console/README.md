# Console firmware (ESP32-S3, CAN node 0x13)

Firmware for the console board ([`ConsoleBoard/`](../../ConsoleBoard/)): the SSD1322 256 × 64 OLED, the two soft
keys under it (**Back** left, **View** right), the **Volume** (left) and **Select** (right) knobs and the key LEDs. It
is the car's radio head unit: sources, the radio, volume, the phone (calls, pairing, the paired phones) and every
accessory's settings, with no touch screen needed. Spec:
[`docs/protocols/can_bus.md`](../../docs/protocols/can_bus.md), *Phone* and *Console*.

```bash
./flash.sh --upload-port /dev/cu.usbmodem…      # build + flash over the board's USB-C
test/host/run_tests.sh                           # UI state machine tests on the Mac (no toolchain needed)
```

| File | |
| --- | --- |
| `src/console_ui.{h,cpp}` | The state machine: CAN frames and key / knob input in, a screen model and CAN commands out. No Arduino code |
| `src/console_display.cpp` | Draws the screen model with U8g2; OLED contrast and key-LED PWM from the cluster brightness |
| `src/console_input.cpp` | Keys (20 ms debounce, act on press) and the encoders (quadrature state table in a GPIO interrupt) |
| `src/console_pins.h` | Pins and input build flags |
| `src/main.cpp` | CAN (`elc_can`), the loop and the serial console |

## Controls

The console is the car's radio head unit. The left knob is the Volume encoder, the right knob the Select encoder; the
two keys under the OLED are soft keys (left = Back, right = View): their labels show over them in a call, while a
phone waits to pair, and for a couple of seconds after any control is touched. Every switch has a click (on release)
and a hold (`CONSOLE_HOLD_MS`, 650 ms).

**At rest** (the drive pages):

| Control | Bluetooth | Radio |
| --- | --- | --- |
| Left knob turn | volume | volume |
| Left knob click | next source: Bluetooth → Radio → Off → Bluetooth (AUDIO_CMD SOURCE to the audio card) | same |
| Left knob hold | the menu | the menu |
| Right knob turn | next / previous track | tune one grid step per detent |
| Right knob click | voice assistant | the preset list (below) |
| Right knob hold | redial | seek up |
| Left soft click | play / pause | mute / unmute |
| Left soft hold | — | store the station as a preset (the next free one; in the preset list, the slot highlighted) |
| Right soft click ("Info") | next OLED page (Media → Trip → Engine → Clock) | same |
| Right soft hold | next view on the center screen | same |

Bluetooth commands go to the audio bridge (`0x215`, as before) and, when an audio card is heard, to it too (AUDIO_CMD
MEDIA / VOLUME); radio commands go to the audio card, which forwards them to the radio
(`docs/protocols/can_channels.md`, AUDIO_CMD). A warning on the drive page is acknowledged with the left soft key.

**The preset list** (right knob click with the radio playing): the twelve presets, four by three, with their
frequencies; the one playing is outlined. Turning the right knob moves the highlight only; closing the list with the
right knob click or the right soft key tunes to the highlighted preset (AUDIO_CMD MEDIA 13, recall). Holding the left
soft key stores the station in the slot highlighted, over what it held. Left alone for 4 s it closes without changing
the station.

**In a call** the soft keys are Decline / Answer, End / Mute (Unmute), Ignore / Answer (holds the active call), End /
Swap; the left knob sets the call volume and its click mutes the microphone. **A phone asking to pair** takes over the
screen with its code: Reject / Pair.

**The menu** (left knob hold) is drawn on the OLED itself; the touch screens' menu is no longer mirrored here. Left
knob turn moves, click opens or changes; left soft is Back (it leaves the menu from the top page), right soft leaves
the menu. In an editor either knob changes the value, a click sends it, the left soft key undoes it.

| Page | What it holds |
| --- | --- |
| Source | Bluetooth, Radio (when a radio card is heard), Off (no source: Bluetooth paused, the radio muted; chimes still sound) |
| Radio | the radio card's own page from the UI channel: on / off, band, frequency, seek, presets, region… |
| Phone | the connected phone, Pair new phone / Stop pairing, voice assistant, redial, the paired phones (Connect or Disconnect, Forget with a confirmation) |
| Accessories | every accessory's page from the UI channel (the audio card, GPS, the gateway…) |
| Display | center view, theme, brightness, dim with lights, menu screen, media screen, right layout, OLED contrast (live, undone with the left soft key) |
| Trip | Trip A, and resetting it |
| Service | the center, the sensors, the audio bridge, the audio card and its source, the radio, the channel nodes |

The key LEDs follow the cluster brightness and pulse while a call rings or a code waits.

## Build flags

| Flag | Default | |
| --- | --- | --- |
| `CONSOLE_OLED_ZJY` | 0 | 1 = U8g2's ZJY init for SSD1322 modules that show garbage with the NHD one |
| `CONSOLE_ENC_STEPS_PER_DETENT` | 4 | quadrature transitions per click (2 or 1 for other encoders) |
| `CONSOLE_SEL_REVERSE` / `CONSOLE_VOL_REVERSE` | 0 | 1 = the knob counts the wrong way |
| `CONSOLE_KEY_LEFT_X` / `CONSOLE_KEY_RIGHT_X` | 45 / 211 | OLED x the soft-key labels centre on (above the keys) |
| `CONSOLE_LED_MAX_PCT` | 100 | key-LED PWM at full brightness |
| `ELC_CAN_TX_PIN` / `ELC_CAN_RX_PIN` | 5 / 6 | TWAI to the MCP2562 (`-1` = CAN off) |

## Serial console (USB, 115200)

For the bench, without knobs wired: `help`, `state` (CAN, phone, the audio card's source and what plays, cluster),
`chan` (the channel node's announced nodes and accessory pages), `soft left|right [hold]`, `left +N|-N|click|hold`,
`right +N|-N|click|hold`. Every
command the console sends is logged as `tx 0x215 phone ANSWER arg 0 slot 0 list 3`.

The console follows the first audio bridge it hears: a node whose heartbeat says audio bridge (node type 1). Its
node follows its card slot (`0x14` for the audio card in slot 1). Until a bridge is heard, phone commands are not
sent at all, and frames in another card's message range are ignored.
