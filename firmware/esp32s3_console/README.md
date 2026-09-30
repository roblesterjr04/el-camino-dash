# Console firmware (ESP32-S3, CAN node 0x13)

Firmware for the console board ([`ConsoleBoard/`](../../ConsoleBoard/)): the SSD1322 256 × 64 OLED, the two keys
under it (**Back** left, **View** right), the **Select** and **Volume** knobs and the key LEDs. It is the only
control for the phone on the audio bridge: answering calls, pairing and managing phones. Spec:
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

## Soft keys

The bottom row names what each key does now; Select press = the right key (except on home, where it is play /
pause). Highest priority first:

| Screen | Left (Back) | Right (View) |
| --- | --- | --- |
| a pairing code to confirm | Reject | Pair |
| incoming call | Decline | Answer |
| call waiting | Ignore | Answer (holds the active call) |
| calling | End | — |
| in a call | End | Mute / Unmute |
| call on hold | End | Swap |
| pairing window open | Stop | — |
| phone menu (paired phones, Pair new phone, Voice assistant, Redial) | Back | Options / Start |
| a phone's options (Connect or Disconnect, Forget) | Back | OK |
| forget, to confirm | Cancel | Forget |
| home | Phone | View (next center view) |

Knobs: Select turns move through lists, and on home skip tracks. Volume sets the call volume during a call and the
media volume otherwise (0–15); its press mutes the microphone during a call and is play / pause otherwise. Menus go
back to home after 20 s untouched; a call covers whatever page was open and returns to it when it ends. The key LEDs
follow the cluster brightness and pulse while a call rings or a code waits.

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

For the bench, without knobs wired: `help`, `state` (CAN, phone status and texts, media, cluster), `screen` (the
current screen as text with its soft-key labels), `key left|right`, `sel +N|-N|press`, `vol +N|-N|press`. Every
command the console sends is logged as `tx 0x215 phone ANSWER arg 0 slot 0 list 3`.

Until an audio bridge is heard, commands go to slot 0 (node `0x10`, the default bridge).
