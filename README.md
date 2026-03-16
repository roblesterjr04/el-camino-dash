# El Camino Dash (Pygame)

Config-first multi-screen dashboard scaffold for Raspberry Pi.

## What this scaffold includes

- 3 display definitions driven by external YAML:
  - 2x round `1080x1080`
  - 1x round `720x720`
- Marlin-style single master config: `config/dashboard.yaml`
- Sensor adapters:
  - Rotary encoder (GPIO)
  - Analog ADC inputs (MCP3008 via SPI)
  - Speed pulse sensor (GPIO edge counting)
  - Keyboard simulation controls for development
- Theme and gauge sprite support with procedural fallback rendering
- Navit/media/Bluetooth metadata integration adapters
- Image-first Raspberry Pi provisioning with `rpi-image-gen` + fast deploy sync script

## Run locally (dev simulation)

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
PYTHONPATH=src DISPLAY_MODE=hardware python src/main.py --config config/dashboard.dev.yaml
```

Keyboard simulation mappings are defined in `inputs.keyboard_sim.mappings` inside config.
`bash scripts/run_sim.sh` still defaults to `config/dashboard.dev.yaml` for a single-window local run.

### Hardware X11 launcher

```bash
bash scripts/run_hw.sh config/dashboard.vehicle.yaml
```

This runs through the same environment bootstrap as `run_sim.sh`, but forces `DISPLAY_MODE=hardware` for X11 desktop sessions.

### Display mode selection

`src/main.py` supports these modes via `--display-mode` or `DISPLAY_MODE`:

- `hardware`: requires `DISPLAY` to be present (recommended default on Pi).
- `auto`: uses `DISPLAY` if present; otherwise exits with guidance.

Headless operation can be forced with `--headless` or `HEADLESS=1` (used for SPI panel output/service mode).

Render topology can be selected via `--render-mode` or `DISPLAY_RENDER_MODE`:

- `single_window`: current compositor behavior (all logical displays in one OS window).
- `multi_window`: one OS window per configured display (X11-oriented).

## Config model

All configurable values are externalized in `config/dashboard.yaml`:

- `system`: app mode, fps, title
  - `system.left_to_right_preview` places all displays in a single left-to-right row for simulation
  - `system.left_to_right_spacing_px` controls spacing between simulated displays
  - `system.menu_schema_path` points to menu definition YAML for the overlay menu
  - `system.menu_display_id` selects which display receives knob-driven menu overlays
- `displays`: shape/resolution/layout/fps/role per screen
  - Optional per-display window mapping keys:
    - `monitor_index` (explicit X11 display index target for multi-window mode)
    - `windowed` / `fullscreen` (window policy for multi-window mode)
- `themes`: active theme, asset roots, fallback policy
  - `display_overrides` in a theme can override `displays` entries by display `id` (applied automatically when that theme is active)
- `gauges`: source mapping + min/max + smoothing
- `inputs`: rotary, analog ADC, speed sensors, keyboard simulation
  - `inputs.simulated_data.enabled` toggles continuous synthetic telemetry for gauge motion
  - `inputs.rotary_encoders` supports multiple encoders (menu + aux)
  - `inputs.keyboard_sim.rotate_repeat_ms` controls keyboard rotary sensitivity
  - `inputs.ir_remote` supports Linux IR key events via `evdev` with profile mappings in `config/remotes/*.yaml`
- `integrations`: Navit/media/Bluetooth feature toggles + settings
  - Optional `integrations.bluetooth_audio` can auto-connect a BT device and route PulseAudio output to it
  - `integrations.fm_radio.chip` selects FM frontend (`si4703` current strategy, `si4737` legacy)
  - `integrations.audio_router` controls PT2314 source selection between Pi DAC and FM feed

The app validates config strictly at startup and exits on schema/key/type errors.

### IR remote profiles

- Profile file example: `config/remotes/remote_1.yaml`
- Select profile in dashboard config:
  - `inputs.ir_remote.profile_path: config/remotes/remote_1.yaml`
- Set either `inputs.ir_remote.device_path` (for example `/dev/input/event2`) or `inputs.ir_remote.device_name`
- Optional protocol lock (recommended for NEC remotes on Pi):
  - `inputs.ir_remote.protocol: nec`
  - `inputs.ir_remote.rc_device: rc0`
  - `inputs.ir_remote.apply_protocol: true`
- Preferred profile format uses `remote.controls` (action -> `KEY_*`), with empty values for unmapped controls
- Diagnostics monitor trigger:
  - 10 quick identical pulses opens IR diagnostics (`inputs.ir_remote.diagnostics_*` controls threshold/timing)
- Action names supported in profile mappings:
  - `menu_left`, `menu_right`, `menu_press`, `aux_left`, `aux_right`, `aux_press`
  - `volume_up`, `volume_down`, `theme_next`, `map_toggle`, `center_toggle`, `tune_up`, `tune_down`

`config/dashboard.dev.yaml` keeps `display_render_mode: single_window` for local development.
`config/dashboard.vehicle.yaml` includes `display_render_mode: multi_window` with explicit `monitor_index` examples.
`config/dashboard.spi.yaml` is kept for legacy/reference SPI setups (currently dormant in installer flow).
`config/dashboard.hdmi5.yaml` targets a single 5" 800x480 HDMI rectangle display.
`WIRING_PINOUT.md` documents current Pi/Arduino/MCP23017/ATtiny pin assignments.

## HDMI 5" run helper

For manual testing in an interactive Pi desktop session:

```bash
DISPLAY=:0 XAUTHORITY=/home/pi/.Xauthority bash scripts/run_hw.sh config/dashboard.hdmi5.yaml
```

## Knob-driven overlay menu

- Menu schema file: `config/menu_schema.yaml`
- Supported row types: `action`, `toggle`, `range`, `submenu`, `info`
- Home-page submenu items render as icon tokens in a horizontal carousel
- Rotary behavior:
  - first press: open full-screen menu overlay on `system.menu_display_id`
  - turn: move through carousel items or list rows
  - press on range: enter/exit adjust mode
  - turn in adjust mode: change value
  - press on action: execute command
- `apply_mode: immediate` applies config changes right away
- `apply_mode: staged` marks unsaved edits; use **Save Config** menu action to persist
- UI includes highlighted selections and list rendering for submenu pages

## Sprite drop-in layout

Place optional sprite files here:

- `assets/gauges/<theme_name>/<gauge_id>/dial.png`
- `assets/gauges/<theme_name>/<gauge_id>/needle.png`
- `assets/gauges/<theme_name>/<gauge_id>/overlay.png`

If any file is missing, that specific layer falls back to procedural drawing.
If theme files are missing, gauges still render with fallback visuals.

Theme files can also override needle mapping and calibration visually per gauge:

- `gauge_visuals.<gauge_id>.visual_min` / `visual_max` (needle sweep bounds)
- `gauge_visuals.<gauge_id>.angle_min_deg` / `angle_max_deg` (physical needle start/end angles)
- `gauge_visuals.<gauge_id>.calibration.scale` / `offset` (value correction)

This lets each theme tune visual ranges without changing global gauge config.

Themes can also override display layout/shape/resolution by display id:

- `display_overrides._all` (optional defaults for all displays)
- `display_overrides.<display_id>` (per-display override, e.g. `width`, `height`, `position`, `enabled`, `shape`, `fps`, `role`, `gauge_id`)

Theme files can define indicator lights per gauge with on/off sprites:

- `indicator_elements.<gauge_id>[]`
  - `source`: value key in runtime map (for example `analog_inputs.fuel_level`)
  - `on_condition.operator`: one of `>`, `>=`, `<`, `<=`, `==`, `!=`
  - `on_condition.value`: threshold target for the operator
  - `on_sprite` / `off_sprite`: sprite filenames in `assets/gauges/<theme>/`
  - `show_when_off`: if `false`, indicator is hidden when off
  - `center_ratio`: `[x, y]` placement in gauge local coordinates
  - `size_ratio`: icon size relative to gauge size

## Raspberry Pi scripts

Provisioning is image-first. Build an image using the in-repo image project:

```bash
bash scripts/build_image.sh --docker
```

Image config and hooks live under `image/el-camino-dash/`.
`scripts/build_image.sh` supports Docker, remote SSH builder, and local Linux modes.

For day-to-day code updates on a provisioned Pi, deploy directly to `/opt/el-camino-dash`:

```bash
bash scripts/deploy_to_pi.sh pi@<host>
```

Optional dependency refresh:

```bash
bash scripts/deploy_to_pi.sh pi@<host> --with-pip
```

Install or refresh tracked runtime services when needed:

```bash
sudo bash scripts/install_runtime_services.sh
```

Image-era verification:

```bash
bash scripts/verify_install.sh
```

## Arduino sketches and flashing

Arduino firmware and helper tooling now live under `arduino/`.

- Entry point docs: `arduino/README.md`
- First sketch: `arduino/sketches/sensor_pack/sensor_pack.ino`
- CLI setup script (Homebrew): `arduino/scripts/install_arduino_cli.sh`
- Compile helper: `arduino/scripts/compile_sensor_pack.sh`
- Upload helper: `arduino/scripts/upload_sensor_pack.sh`

Quick start:

```bash
bash arduino/scripts/install_arduino_cli.sh
bash arduino/scripts/compile_sensor_pack.sh
bash arduino/scripts/upload_sensor_pack.sh /dev/cu.usbmodem101 arduino:avr:uno
```

## Troubleshooting

- Service status: `systemctl status gauge-cluster-hdmi.service`
- Service logs: `journalctl -u gauge-cluster-hdmi.service -f`
- Hardware X11 run helper: `bash scripts/run_hw.sh config/dashboard.vehicle.yaml`
- HDMI 5" run helper:
  - `DISPLAY=:0 XAUTHORITY=/home/pi/.Xauthority bash scripts/run_hw.sh config/dashboard.hdmi5.yaml`
- Legacy SPI config run helper (dormant path):
  - `HEADLESS=1 SDL_VIDEODRIVER=dummy PYTHONPATH=src .venv/bin/python src/main.py --config config/dashboard.spi.yaml --render-mode spi --headless`
- HDMI service status: `systemctl status gauge-cluster-hdmi.service`
- HDMI service logs: `journalctl -u gauge-cluster-hdmi.service -f`
- IR raw pulse monitor: `sudo mode2 -d /dev/lirc0`
- IR decode monitor: `sudo ir-keytable -s rc0 -p nec -t`
- Verify install for Pi hardware mode:
  - `bash scripts/verify_install.sh --target pi --display-mode hardware`
- Validate config manually:
  - `PYTHONPATH=src .venv/bin/python -c "from dashboard.config import load_dashboard_config; load_dashboard_config('config/dashboard.yaml')"`

## Bluetooth audio

Bluetooth audio routing is configured in `integrations.bluetooth_audio`:

- `enabled`: turns Bluetooth audio management on/off
- `device_mac`: target device MAC (`AA:BB:CC:DD:EE:FF`)
- `auto_connect`: connect automatically at app start
- `pair_on_connect`: attempt pairing during connect flow
- `trust_device`: mark device trusted before connect
- `set_default_sink`: set PulseAudio default sink to the BT sink
- `sink_name`: optional explicit PulseAudio sink name (auto-discovered from MAC when empty)
- `refresh_interval_s`: reconnect/status poll interval

The app also exposes command `bluetooth_audio.reconnect` for manual reconnect from menu actions.
