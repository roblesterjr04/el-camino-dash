# Protocols

Wire formats between the boards. Start with [`can_guide.md`](can_guide.md), the explanation of the whole bus; then
[`can_bus.md`](can_bus.md) and [`can_channels.md`](can_channels.md) for the byte layouts.

| File | What it covers | Implemented by |
| --- | --- | --- |
| [`can_guide.md`](can_guide.md) | **The CAN bus explained**: card pins to bus, node numbers from the slot, ID map and priority, heartbeats, transfer framing, discovery, every channel, a card's life, worked frames, compatibility, implementing a node | Everything on the bus; reference code [`firmware/elc_can`](../../firmware/elc_can/) |
| [`can_bus.md`](can_bus.md) | CAN physical layer, node numbers, the cluster's own frames | [`firmware/elc_can`](../../firmware/elc_can/) (IDs, codecs, driver), used by the round displays, the console, the audio bridge and the sensor card firmware |
| [`can_channels.md`](can_channels.md) | The accessory channel protocol (version 1, draft): transport, NET, SYSTEM, ALERT, SENSOR, UI, INPUT, AUDIO_CMD, BULK, ONLINE, LOG | `firmware/elc_can` (`elc_chan`, `accessory_can`) on the round displays, the console and the sensor card. Partly implemented: the doc's status line lists what is not |
| [`cluster_i2c.md`](cluster_i2c.md) | The center-to-side-screens I2C bus (theme sync, registers) | `firmware/esp32_p4_common` (`include/cluster_i2c.h`); the bus is wired on the main board |
| [`nano_uart.md`](nano_uart.md) | The encoder controller's serial frames, written for the shelved Raspberry Pi dashboard | `arduino/sketches/encoder_controller`. Nothing in `firmware/` uses it |
| [`sensor_calibration.md`](sensor_calibration.md) | Sensor card calibration: presets, captured points in FRAM, menu items, console commands, backup | [`firmware/sensor_card`](../../firmware/sensor_card/) (host-tested, not run on a card); [`firmware/sensor_card_stm32`](../../firmware/sensor_card_stm32/) builds the same code. The screen-side changes are not done |
| [`sensor_uart.md`](sensor_uart.md) | The sensor pack's 36-byte serial frame to the screens | `arduino/sketches/sensor_pack` (sender), `firmware/esp32_p4_common` (`sensor_packet.cpp`, receiver). On rev 2 the sensor card's J8 and the main board's J27 carry it as a fallback |
| [`usb_link.md`](usb_link.md) | CAN over the boards' USB serial ports, and the relay | `firmware/elc_can` on every board that runs it; [`tools/can_relay.py`](../../tools/can_relay.py) on the computer |
