# Arduino

Arduino assets and tooling for the El Camino Dash project live here.

## Layout

- `arduino/sketches/` - all Arduino sketches by feature/module.
- `arduino/scripts/install_arduino_cli.sh` - install/check `arduino-cli` with Homebrew.
- `arduino/scripts/compile_sensor_pack.sh` - compile the first sketch.
- `arduino/scripts/upload_sensor_pack.sh` - compile + upload to a board.
- `arduino/scripts/monitor_sensor_pack.py` - decode binary packets into readable text.

## Quick Start (macOS + Homebrew)

Install CLI and AVR core:

```bash
bash arduino/scripts/install_arduino_cli.sh
```

Compile sketch:

```bash
bash arduino/scripts/compile_sensor_pack.sh
```

Upload sketch (example for an Uno on `/dev/cu.usbmodem101`):

```bash
bash arduino/scripts/upload_sensor_pack.sh /dev/cu.usbmodem101 arduino:avr:uno
```

Monitor decoded packets (instead of garbled binary characters):

```bash
python3 arduino/scripts/monitor_sensor_pack.py --port /dev/cu.usbmodem101 --baud 115200
```

## Notes

- Default board (`FQBN`) in scripts is `arduino:avr:uno`.
- Change `FQBN` and serial port for your exact board.
- To discover ports: `arduino-cli board list`.
- To discover valid board IDs: `arduino-cli board listall`.
