# Nano UART Protocol

The Pi and encoder ATmega328P exchange newline-delimited UTF-8 frames on the **primary UART** (Pi **TXD0 / RXD0**, GPIO 14/15), device **`/dev/serial0`** (or `ttyAMA0` on some kernels).

## Shared serial port on the Pi

When `inputs.encoder_arduino_serial` is enabled, the dashboard opens **one** serial device for both:

- **Uplink** (Nano → Pi): encoder lines (`MD=…`).
- **Downlink** (Pi → Nano): OLED lines (`MODE=…|…`).

`NanoBridge` binds to the same port via `InputManager.nano_serial_writer()` so two processes do not fight for the tty. If encoder serial is disabled but `nano_bridge` is enabled, the bridge opens the port alone (bench use).

## Nano -> Pi (input/control)

- Rotary and button events: `MD=<int>,AD=<int>,MP=<int>,AP=<int>`
- Optional **IR** (NEC decoded on the encoder MCU): append `,IR=<token>` with one of:
  `volume_up`, `volume_down`, `theme_next`, `map_toggle`, `center_toggle`, `tune_up`, `tune_down`, `dash_exit`, `menu_open`, `diagnostics_open`
- Menu / aux **navigation** from IR is applied on the MCU (same `MD` / `MP` / `AD` / `AP` counters as hardware); media-style actions use `IR=<token>`.

The Python parser ignores lines that are not encoder frames. Downlink lines are distinguished by containing both `MODE=` and `|`.

### Tuning NEC codes

`encoder_controller.ino` maps NEC **command** bytes in `applyNecCommand()` (default placeholders `0x10`–`0x29`). Watch **Serial** at 115200 for `IR_LEARN nec:a=…:c=0x..` when you press an unknown key, then assign that command in the switch. Keep **`inputs.ir_remote.enabled: false`** on the Pi when using encoder IR so `evdev` and the MCU do not fight the same actions.

## Pi -> Nano (OLED/UI)

- OLED updates: pipe-separated `KEY=value` pairs on one line, newline-terminated.
- **Required:** `MODE`, `TITLE` (menu/theme label), `L1`, `L2` (menu lines / legacy).
- **Drive dashboard (256×64):** `WL` (weather label, e.g. `OUTSIDE`), `WT` (**outside air** from `analog_inputs.ambient_temp`, e.g. `72F`; `--` if no source), `ST` / `SA` (song title / artist), `PR` (0–100 progress), `PE` / `PT` (elapsed / total like `2:15` and `3:45`), `TM` / `AP` (clock `10:23` and `AM`/`PM`). Engine coolant stays on `analog_inputs.coolant_temp` for gauges, not on the OLED weather field.
- Values must not contain `|` or newlines (UTF-8 otherwise; firmware shows ASCII-safe glyphs).
- Frames are throttled by `integrations.nano_bridge.write_interval_s`.
- Firmware accepts lines that contain `MODE=` and `TITLE=` and `|` (see `encoder_controller.ino`).

## Notes

- The Nano is the source of truth for IR keymap and encoder decoding (when implemented on that MCU).
- The Pi consumes logical encoder actions and pushes display state for the **SPI SSD1322 256×64** panel on the encoder board (see `encoder_controller.ino`).
- If the Pi stops sending valid OLED lines for **~3 seconds**, firmware shows an idle **bowtie** graphic until the link returns.
