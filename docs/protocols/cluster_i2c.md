# Cluster and audio I2C

On the DigiDash board the theme and the audio bridge's now-playing data, transport and album art travel over CAN
(`can_bus.md`). The displays accept the theme from either bus. **The audio bridge sections below are bench only:** the
center reads `0x2A` and writes the theme to `0x3E` only when built with `CENTER_AUDIO_I2C=1` (its `*-bench` envs), and
the bridge starts those slaves only in its `esp32dev_bench` env (or the plain arduino-cli build, which has no CAN).

## Cluster bus (theme sync)

A dedicated bus on GPIO 47 (SDA) / 48 (SCL), 100 kHz, with 3.3 V pull-ups. Master: center (`TwoWire(1)`, 20 ms timeout). Constants live in `firmware/esp32_p4_common/include/cluster_i2c.h`.

| Slave | Address |
| --- | --- |
| Speedo | `0x3C` |
| TSPS | `0x3D` |
| ESP32-D audio bridge (optional; acknowledges and ignores) | `0x3E` |

Each write is `[register][value]`:

| Register | Value |
| --- | --- |
| `0x01` | Theme: 0 Classic, 1 Fallout |
| `0x02` | Center sub-view: 0 Tach, 1 Clock, 2 Media |

The center sends both registers at boot, after each gesture, and **every 5 s**, so a display that booted late picks up the current state. Slaves record the value in their I2C callback and write NVS from `loop()`, and only when the value changed.

## Audio bridge (bench only: ESP32-D @ 0x2A, on the center's display/touch bus GPIO 7/8)

The center writes the register byte `0x0F`, then reads 48 bytes, every 250 ms (25 ms timeout):

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 1 | Now-playing length (≤ 32) |
| 1 | 32 | "Artist - Title" (or title), zero padded |
| 33 | 1 | `0xA5` marker |
| 34 | 1 | `5` (transport block length) |
| 35 | 2 | Elapsed seconds, little-endian |
| 37 | 2 | Duration seconds, little-endian |
| 39 | 1 | Volume 0–15 |
| 40 | 8 | Reserved (0) |

Legacy registers read subsets of the same buffer: `0x01` returns bytes 0–32 and `0x05` returns bytes 34–39.

The bridge rebuilds this buffer whenever AVRCP state changes and every 250 ms. The I2C request handler only copies it. On phone disconnect the metadata is cleared. Elapsed time is estimated between AVRCP position updates and stops while paused.
