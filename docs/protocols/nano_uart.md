# Encoder controller UART (Nano / Pico)

> **Status:** this protocol was written for the shelved Raspberry Pi dashboard, and the Pi-side sections below (YAML keys, `/dev/ttyAMA4`, `NanoBridge`) no longer exist. The encoder firmware (`arduino/sketches/encoder_controller`) still speaks it. Nothing in `firmware/` consumes it yet; the likely host is a spare UART on the center ESP32-P4. The frame formats below still describe what the encoder sends and accepts.

The Pi and encoder ATmega328P exchange newline-delimited UTF-8 frames on the **serial device configured in YAML** (vehicle install: **UART4**, e.g. `/dev/ttyAMA4` — not necessarily the primary `/dev/serial0`). Baud **115200** unless you change both sides.

## Shared serial port on the Pi

**Device path:** Use the same `port` in `inputs.encoder_arduino_serial` and `integrations.nano_bridge` (vehicle default **`/dev/ttyAMA4`** for UART4). Many Pi OS images do **not** provide `/dev/serial0`; if the config still says `serial0` and that node is missing, older builds could fall back to **`/dev/ttyAMA0`** and read the **wrong** UART while the Pico stays on **ttyAMA4**. Override with env **`EL_CAMINO_ENCODER_SERIAL_PORT`** or fix YAML. Watch stdout for `[encoder-serial] opened …` and `[encoder-serial] frame: MD=…`.

When `inputs.encoder_arduino_serial` is enabled, the dashboard opens **one** serial device for both:

- **Uplink** (Nano → Pi): encoder lines (`MD=…`).
- **Downlink** (Pi → Nano): OLED lines (`MODE=…|…`).

`NanoBridge` binds to the same port via `InputManager.nano_serial_writer()` so two processes do not fight for the tty. If encoder serial is disabled but `nano_bridge` is enabled, the bridge opens the port alone (bench use).

## Nano -> Pi (input/control)

- Rotary and button events: **`MD`** / **`AD`** use **`UP`** / **`DOWN`** per detent (repeat for multi-step frames, e.g. `MD=UP,MD=UP,AD=0`), or **`0`** when idle; legacy firmware may still send signed integers. **`MP`** / **`AP`** remain press **counts**: `MP=<int>,AP=<int>`
- Optional **IR** (NEC decoded on the encoder MCU): append `,IR=<token>` with one of:
  `volume_up`, `volume_down`, `theme_next`, `map_toggle`, `center_toggle`, `tune_up`, `tune_down`, `dash_exit`, `menu_open`, `diagnostics_open`
- Menu / aux **navigation** from IR is applied on the MCU (same `MD` / `MP` / `AD` / `AP` counters as hardware); media-style actions use `IR=<token>`.

The Python parser ignores lines that are not encoder frames. Downlink lines are distinguished by containing both `MODE=` and `|`.

### Tuning NEC codes

`encoder_controller.ino` maps NEC **command** bytes in `applyNecCommand()` (default placeholders `0x10`–`0x29`). Watch **Serial** at 115200 for `IR_LEARN nec:a=…:c=0x..` when you press an unknown key, then assign that command in the switch. Keep **`inputs.ir_remote.enabled: false`** on the Pi when using encoder IR so `evdev` and the MCU do not fight the same actions.

## Pi -> Nano (OLED/UI)

- OLED updates: pipe-separated `KEY=value` pairs on one line, newline-terminated.
- **Required:** `MODE`, `TITLE` (menu/theme label), `L1`, `L2` (menu lines / legacy).
- **`MODE=menu`:** When `system.menu_render_on_oled` is true (default in bundled configs), the Pi sends a **vertical scroll list** on the OLED: `TITLE` = page title (shown in the top bar); **`M0`…`M2`** = up to three visible row **labels** (left); **`V0`…`V2`** = optional **status/value** strings for those rows (right-justified before the scrollbar). If a `Vn` is empty or omitted, the firmware treats `Mn` as a single full-width line (backward compatible). **`MT`** = total item count; **`MO`** = index of the first visible row (scroll position); **`MG`** = global selected row index (`0`…`MT-1`). The encoder firmware draws a title bar, three stacked rows (icon bullet + label/value), a rounded selection frame, and a scrollbar when `MT > 3`. If `MT` is absent or zero, the firmware falls back to the legacy three-line layout (`TITLE` / `L1` / `L2` only). The gauge cluster does not draw the menu overlay when `menu_render_on_oled` is true. With `menu_render_on_oled: false`, the cluster keeps the on-screen menu and the Pi sends `MODE=drive` on the UART while the menu is open (OLED stays on the drive layout).
- **Drive dashboard (256×64):** `WL` (weather label, e.g. `OUTSIDE`), `WT` (**outside air** from `analog_inputs.ambient_temp`, e.g. `72F`; `--` if no source), `ST` / `SA` (song title / artist), `PR` (0–100 progress), `PE` / `PT` (elapsed / total like `2:15` and `3:45`), `TM` / `AP` (clock `10:23` and `AM`/`PM`). Engine coolant stays on `analog_inputs.coolant_temp` for gauges, not on the OLED weather field.
- Values must not contain `|` or newlines (UTF-8 otherwise; firmware shows ASCII-safe glyphs).
- Frames are throttled by `integrations.nano_bridge.write_interval_s`.
- Firmware accepts lines that contain `MODE=` and `TITLE=` and `|` (see `encoder_controller.ino`).

## Notes

- The Nano is the source of truth for IR keymap and encoder decoding (when implemented on that MCU).
- The Pi consumes logical encoder actions and pushes display state for the **SPI SSD1322 256×64** panel on the encoder board (see `encoder_controller.ino`).
- If the Pi stops sending valid OLED lines for **~3 seconds**, firmware shows an idle **bowtie** graphic until the link returns.
