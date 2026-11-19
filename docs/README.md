# Documentation

Specifications for the dash: the boards and their mechanical limits in [`hardware/`](hardware/README.md), the wire
formats between boards in [`protocols/`](protocols/README.md). Each board directory also has its own README with
its schematic blocks, pin map, status and how to regenerate it; [`HARDWARE.md`](../HARDWARE.md) lists them all.

## Building your own main board or card

Two guides are written for that, and are the place to start:

- [`hardware/main_board_guide.md`](hardware/main_board_guide.md): the contract a main board must meet so every card
  fits: the slot pinout pin by pin, CAN wiring and termination, slot-ID resistors and node numbers, power and WAKE,
  the I2S rail, the screen headers, minimum boards and a bring-up checklist.
- [`protocols/can_guide.md`](protocols/can_guide.md): the CAN bus explained end to end: from the card's pins to the bus,
  node numbers, the ID map and priority, heartbeats, the transfer framing, discovery, every channel, a card's life
  from power-on, worked frames, compatibility rules and how to implement a node.

## Reading order

For a newcomer, in this order:

1. **The system.** The root [`README.md`](../README.md) (what the dash does, which boards exist) and
   [`HARDWARE.md`](../HARDWARE.md) (every board, which revision is current).
2. **The CAN bus.** [`protocols/can_guide.md`](protocols/can_guide.md) for the explanation, then [`protocols/can_bus.md`](protocols/can_bus.md): the physical layer, node numbers and the
   cluster's own messages. Then [`protocols/usb_link.md`](protocols/usb_link.md): how the bench runs the same bus
   over USB with no transceivers.
3. **Channels.** [`protocols/can_channels.md`](protocols/can_channels.md): the accessory protocol on CAN (alerts,
   sensors, menu pages, audio commands, the online channel). A new card is written against this.
4. **Slots and cards.** [`hardware/accessory_ports.md`](hardware/accessory_ports.md): the slot connector, card size
   and the rules every card follows. Then the card specs: [`hardware/gps_card.md`](hardware/gps_card.md),
   [`hardware/radio_card.md`](hardware/radio_card.md), [`hardware/lte_card.md`](hardware/lte_card.md), and the
   sensor card's [`protocols/sensor_calibration.md`](protocols/sensor_calibration.md).
5. **Mechanics and ordering**, when you get there: [`hardware/housing_envelope.md`](hardware/housing_envelope.md),
   [`hardware/cable_harnesses.md`](hardware/cable_harnesses.md),
   [`hardware/panel_order_costing.md`](hardware/panel_order_costing.md).

## Hardware

| File | What it specifies |
| --- | --- |
| [`main_board_guide.md`](hardware/main_board_guide.md) | **Designing your own main board**: the slot interface every card relies on, CAN, slot IDs, power, screens, bring-up |
| [`accessory_ports.md`](hardware/accessory_ports.md) | Rev 2's six card slots (2×15 socket, pinout, 80 × 60 mm card envelope, slot ID, I2S rail, AUX 12 V) and rev 1's accessory ports and cable ports |
| [`cable_harnesses.md`](hardware/cable_harnesses.md) | Every cable the dash needs: board connector, mating housing, terminals, wire gauge |
| [`digidash_rev2_plan.md`](hardware/digidash_rev2_plan.md) | Why and how the main board was redrawn from rev 1 to rev 2: SMD conversion, sensor pack to a card, video decoder on the board |
| [`gps_card.md`](hardware/gps_card.md) | The GPS card: remote puck, its connector, and the position / time messages it sends on CAN |
| [`housing_envelope.md`](hardware/housing_envelope.md) | Where the main board, the screens and each card sit, in one coordinate frame, and what has to reach outside; notes on the housing model |
| [`lte_card.md`](hardware/lte_card.md) | The LTE gateway card: ESP32-S3 plus an LTE core board, the online work it does, its firmware plan |
| [`panel_order_costing.md`](hardware/panel_order_costing.md) | Where the money goes in a fab order: parts, assembly fees, duties, and cheaper scenarios |
| [`radio_card.md`](hardware/radio_card.md) | The AM/FM radio card: Si4735 tuner on the I2S rail, antenna, control over CAN, the simulator |

## Protocols

| File | What it specifies |
| --- | --- |
| [`can_bus.md`](protocols/can_bus.md) | The CAN bus: 500 kbit/s, node numbers, termination, the cluster's own frames (sensor frames, theme, media, phone, settings) |
| [`can_channels.md`](protocols/can_channels.md) | The accessory channel protocol on CAN: transport, NET, SYSTEM, ALERT, SENSOR, UI, INPUT, AUDIO_CMD, BULK, ONLINE, LOG |
| [`cluster_i2c.md`](protocols/cluster_i2c.md) | The I2C bus between the center screen and the two side screens (theme sync) |
| [`nano_uart.md`](protocols/nano_uart.md) | The encoder controller's serial protocol, from the shelved Raspberry Pi dashboard |
| [`sensor_calibration.md`](protocols/sensor_calibration.md) | How the sensor card turns ADC counts into readings, where the calibration is stored and how it is set and backed up |
| [`sensor_uart.md`](protocols/sensor_uart.md) | The sensor pack's 36-byte serial frame to the screens (115200 8N1, every 50 ms) |
| [`usb_link.md`](protocols/usb_link.md) | CAN over USB: how boards carry their CAN traffic over the USB serial port to `tools/can_relay.py` |

## Elsewhere

- [`WIRING_PINOUT.md`](../WIRING_PINOUT.md) at the repository root: the vehicle harness (sensor pack, encoder
  controller, the round displays, the audio bridge). It describes the rev 1 wiring.
- [`firmware/README.md`](../firmware/README.md): the round displays' firmware (themes, gestures, gauge faces).
