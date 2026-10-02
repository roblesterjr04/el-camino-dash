# Cluster I2C

On the DigiDash board the theme and the audio bridge's now-playing data, transport and album art travel over CAN
(`can_bus.md`). The side displays accept the theme from either bus. The audio bridge has no I2C link: the old bench
builds that read it at `0x2A` (and mirrored the theme to `0x3E`) are gone; the bench uses the USB CAN link
(`tools/can_relay.py`, [`usb_link.md`](usb_link.md)) instead.

## Cluster bus (theme sync)

A dedicated bus on GPIO 47 (SDA) / 48 (SCL), 100 kHz, with 3.3 V pull-ups. Master: center (`TwoWire(1)`, 20 ms timeout). Constants live in `firmware/esp32_p4_common/include/cluster_i2c.h`.

| Slave | Address |
| --- | --- |
| Speedo | `0x3C` |
| TSPS | `0x3D` |

Each write is `[register][value]`:

| Register | Value |
| --- | --- |
| `0x01` | Theme: 0 Classic, 1 Fallout |
| `0x02` | Center sub-view: 0 Tach, 1 Clock, 2 Media |

The center sends both registers at boot, after each gesture, and **every 5 s**, so a display that booted late picks up the current state. Slaves record the value in their I2C callback and write NVS from `loop()`, and only when the value changed.
