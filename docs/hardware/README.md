# Hardware specifications

Specs and plans that span more than one board. Each board's own README (listed in
[`HARDWARE.md`](../../HARDWARE.md)) has its schematic, pin map and status.

| File | What it covers | Implemented by |
| --- | --- | --- |
| [`accessory_ports.md`](accessory_ports.md) | Rev 2 card slots: the 2×15 slot pinout, card envelope, slot ID, I2S rail, AUX 12 V, cable and trunk ports. Rev 1 accessory ports (2×5 headers) | Boards: [`DigiDash2`](../../DigiDash2/), [`DigiDash3`](../../DigiDash3/), [`BenchBoard`](../../BenchBoard/), every slot card. Rev 1: [`DigiDash`](../../DigiDash/) |
| [`cable_harnesses.md`](cable_harnesses.md) | The mating cables for every board connector: housings, terminals, wire gauge | Cables for [`DigiDash2`](../../DigiDash2/), the cards and [`ConsoleBoardRS`](../../ConsoleBoardRS/) |
| [`digidash_rev2_plan.md`](digidash_rev2_plan.md) | The plan for main board rev 2: what rev 1 did badly, SMD conversion, the sensor pack moved to a card, the video decoder on the board | Board: [`DigiDash2`](../../DigiDash2/) (and [`DigiDash3`](../../DigiDash3/), its through-hole copy). Firmware steps in it: not done |
| [`gps_card.md`](gps_card.md) | The GPS card: puck connector, CAN messages (position, motion, quality, time, status) | Boards: [`GPSCard`](../../GPSCard/), [`GPSCard2`](../../GPSCard2/). Firmware: not written |
| [`housing_envelope.md`](housing_envelope.md) | Positions and heights of the main board, screens and cards in one frame; openings; the housing model's features and collision test | [`Housing/cluster_housing.fs`](../../Housing/) |
| [`lte_card.md`](lte_card.md) | The LTE gateway card: ESP32-S3, LTE core board, ONLINE channel, Wi-Fi access point, firmware plan | Board: [`LTECard`](../../LTECard/). Firmware: not written |
| [`panel_order_costing.md`](panel_order_costing.md) | Cost of a fab order: parts, assembly fees, duties, scenarios; the case for the through-hole rev 3 | [`Panel`](../../Panel/), `Panel/fab/Panel_BOM_resourced.csv`, [`DigiDash3`](../../DigiDash3/) |
| [`radio_card.md`](radio_card.md) | The AM/FM radio card: Si4735 on the I2S rail, antenna input, control over CAN, the simulated card | Board: [`RadioCard`](../../RadioCard/). Firmware: not written; `tools/radio_sim.py` stands in on the bench |
