# Sensor pack UART

`arduino/sketches/sensor_pack/sensor_pack.ino` → all three ESP32-P4 displays (`firmware/esp32_p4_common/src/sensor_packet.cpp`). Bench decoder: `arduino/scripts/monitor_sensor_pack.py`.

## Link

- 115200 8N1, every 50 ms. Sensor TX fans out to each display's RX (GPIO 5).
- Only the **center** transmits back (GPIO 4 → sensor RX): one `0xA5` keepalive every 350 ms (`SENSOR_LINK_KEEPALIVE=1`). The sensor pack uses it only for its link LED and to time its FRAM status message. Speedo and TSPS never write to the UART, so wiring their TX pins together can't cause contention.

## Frame (36 bytes, big-endian)

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 2 | Sync `AA 55` |
| 2 | 2 | Fuel sender ADC (0–1023, EMA filtered) |
| 4 | 2 | Engine temp thermistor ADC |
| 6 | 2 | Oil pressure ADC |
| 8 | 2 | Charge band (synthesized from IGN + ALT "L": 400 key off, 530 ign on / not charging, 720 charging) |
| 10 | 2 | Ambient thermistor ADC |
| 12 | 4 | Wheel RPM (hall pulses per rev already divided out) |
| 16 | 4 | Engine RPM |
| 20 | 1 | Flags: bit0 ALT OK, bit1 IGN ON |
| 21 | 2 | Fault word (below) |
| 23 | 4 | Odometer, tenths of a mile. `0xFFFFFFFF` = unknown (FRAM unreadable) |
| 27 | 2 | Instant MPG × 10 (0 = n/a) |
| 29 | 2 | Trip-average MPG × 10 since key-on (0 = n/a) |
| 31 | 4 | Distance to empty, tenths of a mile (20 gal tank model) |
| 35 | 1 | XOR of the 33 payload bytes (offsets 2–34) |

The parser only accepts complete 36-byte frames with a valid XOR. The older 19/21/25-byte layouts are no longer accepted.

## Text lines

The sensor pack also prints short ASCII lines, CR/LF terminated, **between** frames:
- the boot banner (`------` … `######`, then 6-digit odometer tenths, or `??????` if unknown)
- FRAM status such as `FRAM rd v3 OK` / `t=966486`, `FRAM wr 10th`, `FRAM rd FAIL` / `no chip`

Receivers queue these (`takePendingTextLine`). The speedo shows them briefly in the odometer slot, and the center logs them.

## Fault word

| Bit | Name | Meaning |
| --- | --- | --- |
| 0 / 1 | POT_OPEN / POT_SHORT | Fuel sender (2-wire mode only) |
| 2 / 3 | TEMP_OPEN / TEMP_SHORT | Engine temp |
| 4 / 5 | PRESS_OPEN / PRESS_SHORT | Oil pressure |
| 6 / 7 | VOLT_OPEN / VOLT_HIGH | Reserved |
| 8 | HALL_MISSING | Off by default (`SENSORPACK_HALL_MISSING_FAULT`); it can't be told apart from idling at a stop |
| 9 | COIL_MISSING | Ignition on, no coil pulses for 5 s (engine not running) |
| 10 | ALT_STUCK | Alternator L never changed state while running |
| 11–14 | *_POWER | Sensor-side fuse monitor low |
| 15 | ODO_STORE | FRAM unreadable or corrupt: odometer unknown and **not being saved** |

## Odometer storage (FRAM MB85RC @ 0x50)

- **v3 layout:** two 20-byte slots, A at `0x10` and B at `0x30`: `E1 D0 03 00 | seq u32 | tenths u32 | frac_nano u32 | 3 reserved | CRC-8 (poly 0x07)`.
- Writes alternate between slots, and boot loads the valid slot with the newest sequence number, so power loss mid-write can't corrupt the stored mileage.
- Stamped on every 0.1 mi, plus the sub-tenth fraction every ~2 s while moving.
- Legacy v1/v2 blobs at address 0 are migrated on first boot.
- The factory seed (`ODO_INITIAL_TENTHS`) is written **only** when the chip ACKs and the whole odometer area reads blank (all `00` or all `FF`).
- On any read failure the odometer is unknown: nothing is written, bit 15 is set, and loading is retried every 5 s. Distance driven in the meantime is added once the stored value loads.
