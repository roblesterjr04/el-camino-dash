# Sensor Pack I2C Frame

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
