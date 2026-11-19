# DigiDash main board, rev 1 (superseded)

**Status: superseded by [`DigiDash2/`](../DigiDash2/README.md).** Kept as the reference that rev 2 was planned from
([`docs/hardware/digidash_rev2_plan.md`](../docs/hardware/digidash_rev2_plan.md), "What rev 1 does badly"). Not on the
panel. Whether it was ever ordered or built: not stated.

**Firmware:** the on-board sensor pack runs [`arduino/sketches/sensor_pack`](../arduino/sketches/sensor_pack/); the
screens run [`firmware/esp32_p4_display`](../firmware/esp32_p4_display/).

The first main board: 309.2 × 103.7 mm, 2 layers, every part through-hole. On it:
- the three screen headers (2×20) for the Waveshare ESP32-P4 round displays, with an MCP2562 CAN transceiver per screen
- an **ATmega328P** (DIP-28) sensor pack whose serial output fans out to the three screens
  ([`docs/protocols/sensor_uart.md`](../docs/protocols/sensor_uart.md))
- plug-in modules: an FRAM board for the odometer and a TVP5150 video decoder board for the reversing camera
- an LM2576 5 V switching regulator
- four 2×5 accessory ports (J18–J21) and cable ports on the CAN bus
  ([`docs/hardware/accessory_ports.md`](../docs/hardware/accessory_ports.md), rev 1 sections)

| File | What |
| --- | --- |
| `DigiDash.kicad_sch` / `.kicad_pcb` / `.kicad_pro` | The board. There is no `generate/` folder; it was autorouted with Freerouting 2.1 (git history) |
| `DigiDash_Accessory.pretty` | The 2×5 accessory port and carrier header footprints. **Still in use:** DigiDash2, DigiDash3 and AudioCarrier list this library in their `fp-lib-table`, so keep the folder |
| `freerouting.dsn` | The Specctra export from the last autoroute |

The schematic uses symbols from KiCad's PCM SparkFun libraries (power symbols, a capacitor, a crystal) and an
`RCJ-022` symbol that is not in KiCad's stock libraries. Install those to open it without missing-symbol warnings.
