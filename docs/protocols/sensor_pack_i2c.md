# Sensor Pack I2C Frame (legacy)

> **Vehicle / production:** the **sensor ATmega328P is 5V**; shipping configs use **UART only** (`inputs.arduino_serial`, `sensor_pack.ino` binary frames on **Pi UART1**), **not** an I2C link to the Pi. See **`src/dashboard/input/arduino_serial.py`**.

This document describes the **optional** I2C register map for **bench-only** or **3.3V I2C-slave** sensor builds consumed by `SensorPackI2CInput`.

---

ATmega328 sensor pack publishes a fixed frame on I2C. Pi polls register `0x00`.

## Frame Layout (16 bytes, big-endian)

- `0..1`: speed mph x10
- `2..3`: engine rpm
- `4..5`: fuel percent x10
- `6..7`: coolant temp F x10
- `8..9`: oil pressure psi x10
- `10..11`: system voltage V x10
- `12`: bit flags (`bit0=alt_ok`, `bit1=ign_on`)
- `13..14`: fault word
- `15`: reserved/crc

This frame is consumed by `SensorPackI2CInput` and mapped into dashboard value keys.
