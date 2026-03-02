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
- Raspberry Pi install scripts + `systemd` service script

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
- `integrations`: Navit/media/Bluetooth feature toggles + settings

The app validates config strictly at startup and exits on schema/key/type errors.

`config/dashboard.dev.yaml` keeps `display_render_mode: single_window` for local development.
`config/dashboard.vehicle.yaml` includes `display_render_mode: multi_window` with explicit `monitor_index` examples.
`config/dashboard.spi.yaml` targets 3x GC9A01 displays on SPI1 (`display_render_mode: spi`).

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

For new SPI1 breadboard setups, run the top-level installer:

```bash
sudo bash install.sh
```

This configures `dtoverlay=spi1-3cs`, sets `spidev.bufsiz=65536`, installs dependencies, creates `~/gauge-cluster`, and installs/enables `gauge-cluster.service`.

Legacy install scripts are still available and run in this order:

```bash
sudo bash scripts/install_pi_base.sh
bash scripts/install_app.sh
sudo bash scripts/install_service.sh
bash scripts/verify_install.sh
```

Each legacy install script supports `--dry-run`.

## Troubleshooting

- Service status: `systemctl status el-camino-dash.service`
- Service logs: `journalctl -u el-camino-dash.service -f`
- Hardware X11 run helper: `bash scripts/run_hw.sh config/dashboard.vehicle.yaml`
- SPI config run helper:
  - `HEADLESS=1 SDL_VIDEODRIVER=dummy PYTHONPATH=src .venv/bin/python src/main.py --config config/dashboard.spi.yaml --render-mode spi --headless`
- SPI service status: `systemctl status gauge-cluster.service`
- SPI service logs: `journalctl -u gauge-cluster.service -f`
- Verify install for Pi hardware mode:
  - `bash scripts/verify_install.sh --target pi --display-mode hardware`
- Validate config manually:
  - `PYTHONPATH=src .venv/bin/python -c "from dashboard.config import load_dashboard_config; load_dashboard_config('config/dashboard.yaml')"`
