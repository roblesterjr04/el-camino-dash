# LTE gateway card

**Status: drawn, not ordered.** Schematic and routed 4-layer board in [`LTECard/`](../../LTECard/README.md), made by
generator scripts in the audio card's pattern (`LTECard/generate/`). The core board's pin order and row spacing in it are
still the provisional ones below: measure the board in hand before ordering (see the list in `LTECard/README.md`).

A slot card that gives the dash internet. An **ESP32-S3** talks to an **LTE Cat-1 core board** (a ready-made
breakout, plugged in on pin headers) and **does the dash's online work itself**: street name, weather, time zone,
incoming SMS. It broadcasts only the results on CAN (the ONLINE channel, `docs/protocols/can_channels.md`), so the
screens never need Wi-Fi for these. A Wi-Fi access point is there too, **off by default**: turned on from the menu for
a phone in the car or a bulk download (screen firmware), and the center can then join it on its own (NET gateway).

```
                                                          ┌── CAN: ONLINE (place, weather, time zone, messages, status),
 LTE antenna ── IPEX ── LTE core board ══ UART (PPP) ══ ESP32-S3    NET (ANNOUNCE, GATEWAY STATUS, WIFI OFFER)
                                                          │      ◄── CAN: GPS position (0x222) from the GPS card
                                                          └── Wi-Fi access point (optional) ── phone, bulk downloads
```

## The LTE core board

The board in hand looks like a **SIMCom A7670-series core board** (the A7670 module is on its underside). From its
silkscreen and the vendor listings for those boards:

| Pin | What | Use on this card |
| --- | --- | --- |
| VIN, VIN | Power in, **5–16 V**, for the board's own buck converter (the 4R7 inductor) | The card's own 5 V from AUX 12 V (see [Power](#power)) |
| GND (several) | Ground | GND |
| VBTA | Battery-level input (3.4–4.2 V) straight to the module, bypassing the board's buck | Not connected |
| VTTL | Reference for the board's level shifter (TXS0108E): sets the UART's logic level | **3.3 V** from the card |
| VEXT | The module's 1.8 V output | Not connected (STA says the module is on) |
| 5V | Not documented on the listings; likely the USB 5 V | Not connected |
| TXD, RXD | Module UART (named from the module: TXD is its output) | ESP32 RX ← TXD, ESP32 TX → RXD |
| RTS, CTS | Hardware flow control | Both wired: PPP needs flow control at high baud rates |
| DTR | Sleep control (low = awake) | ESP32 output, held low |
| RI | Ring / event indicator | ESP32 input (can wake the ESP32) |
| PEN | Power enable for the board | ESP32 output: the module is off until the firmware raises it |
| RST | Module reset | ESP32 output |
| STA | Power-on status | ESP32 input |
| NET | Network status (LED drive) | ESP32 input (blink pattern = registration state) |
| IO0, IO1 | Module GPIOs | Not connected |
| SR, SD, SC | External SIM (reset, data, clock) | Not connected: the board has its own SIM slot |

**Before drawing the schematic, check against the board in hand:**
- The pin count and order. The listings say 23 pins at 2.54 mm: probably 12 down one short edge and 11 down the other.
- The **row-to-row spacing**. Probably 40.64 mm (16 × 2.54): measure it. The listings give the board as 44.45 × 31.75 mm,
  12.44 mm tall including pins.
- The PEN and RST polarity, and whether PEN has a pull-up on the board (the listings don't say).
- Where the IPEX antenna connector is (most likely on the underside, next to the module).

The card's header positions and pin map are constants in its generator, so a different core board is a small edit.

## Parts

| Part | Choice | Why |
| --- | --- | --- |
| MCU | **ESP32-S3-WROOM-1-N16R8** (16 MB flash, 8 MB PSRAM) | Wi-Fi access point and routing (NAT) in one chip, PSRAM for the network buffers, native USB for programming. Its PCB antenna sits at the card's top-left corner with a keep-out. Option: the **-1U** variant with an IPEX lead to a Wi-Fi antenna on the case panel, if the case is metal |
| LTE | The core board, on **two female pin sockets** (1 × 12 and 1 × 11, 2.54 mm, 8.5 mm tall) | Removable. The 8.5 mm sockets leave room under it for the A7670 and the IPEX plug |
| CAN transceiver | MCP2562 (VDD 5 V, VIO 3.3 V, STBY low) | As every other node. No termination |
| 3.3 V | AP7361C-33 (1 A) from the slot's 5 V | The ESP32-S3 peaks at about 500 mA with Wi-Fi transmitting |
| LTE 5 V | **TPS54560B buck** (the main board's 5 V circuit, smaller inductor) from **AUX 12 V** (slot pins 25–30), plus 470 µF bulk at the core board's VIN | LTE transmit bursts reach about 2 A. That would trip the slot's 0.5 A 5 V fuse, so the LTE side runs from AUX. The buck also keeps the core board's 16 V limit safe from the car's spikes |
| Programming | **USB-C, mid-mount, on the card's left edge**, to the ESP32-S3's native USB, with BOOT and RESET buttons beside it | Required: the ESP32 comes from assembly blank. Reachable with the card in its slot. USB-C also gives a serial console |
| Debug serial | 1 × 3, 2.54 mm, right angle, beside the USB-C: GND, TX0, RX0 | Second way to flash and log |
| Indicators | Two LEDs: LTE (off / searching / data up), Wi-Fi (access point on, blinks with clients) | |

**Slot pins used:** 1 / 2 (+5 V), GND, 5 / 6 (CAN), 11 (slot ID), 25–30 (AUX 12 V / AUX GND). Everything else is left
unconnected.

### ESP32-S3 pins (to fix in the schematic)

| Function | Signals |
| --- | --- |
| LTE UART (UART1) | TX, RX, RTS, CTS |
| LTE control | PEN, RST, DTR (outputs); STA, NET, RI (inputs) |
| CAN (TWAI) | TX, RX |
| Slot ID | an ADC1 input with 10k to 3.3 V |
| LEDs | 2 outputs |
| AUX present | an input through a divider from the buck's output (tells "no AUX power" from "no signal") |

No pull-ups on the LTE UART lines on this card: the core board's TXS0108E level shifter does not like them.

## Power

- **ESP32 and CAN:** from the slot's 5 V, so the card always reports on CAN, even without AUX.
- **LTE:** from AUX 12 V through the card's own buck to 5 V on the core board's VIN.
- **With the key off:** AUX is whatever feeds J13. On an ignition-switched fuse the gateway is off with the key (version
  1). Uploads with the key off would need J13 on a constant fuse, PEN low while idle, and a WAKE / timer scheme. That is
  for later.

## Board

- **80 × 60 mm**, any slot. Parts side up to 15 mm. The core board on its 8.5 mm sockets comes to about 14 mm (8.5 +
  1.6 + its 4 mm tallest parts, the inductor and the USB socket): check with the board in hand.
- **Placement (parts side):** the core board in the upper right, its SIM slot facing out (swapping the SIM means pulling
  the card); the ESP32-S3 module in the upper left with its antenna end on the top edge, away from the LTE antenna lead;
  the buck, regulator and CAN transceiver along the bottom, by the slot header; the USB-C and buttons on the left edge.
- **Antenna:** a short IPEX-to-SMA bulkhead pigtail from the core board to the case panel, then an external LTE antenna
  (roof, windshield or under the dash plastic). Nothing RF on the card itself.
- **Layers:** 4, the same rules as the audio card. The buck's input loop tight, its ground into the AUX return.

## Firmware

ESP-IDF (or Arduino-ESP32 3.x, which wraps the same parts):

1. **LTE up:** PEN high, wait for STA, then AT commands. Raise the baud rate with RTS / CTS on, check the SIM and
   registration, set the APN (a setting, kept in NVS).
2. **Data:** PPP over the UART (the `esp_modem` component) with **CMUX**, so AT status queries keep working while data
   flows.
3. **Online services, results on CAN (ONLINE channel):** it listens to the GPS card's position on CAN and, when the
   car has moved 50 m and 10 s have passed, looks up the street (OpenStreetMap Nominatim) and broadcasts PLACE. Weather
   and the time zone (Open-Meteo) every 15 minutes. Incoming SMS (AT+CMGR) as MESSAGE. ONLINE STATUS once a second.
   HTTPS keeps one connection open per service, so each lookup costs about 1 KB, not a new TLS handshake.
4. **CAN (node `0x06`, product `0x0006`):** heartbeat `0x706` (node type 7); NET ANNOUNCE with the slot; GATEWAY
   STATUS once a second (signal from AT+CSQ, registration from AT+CEREG, data used from the PPP byte counts); act on
   GATEWAY CMD, REFRESH and MESSAGE READ.
5. **Wi-Fi (optional, off by default):** an access point with a random password made on first boot (kept in NVS),
   turned on by GATEWAY CMD or the menu. lwIP NAT (NAPT) routes its clients out through PPP; DHCP gives them the card's
   address as their DNS, and it forwards DNS. Answers WIFI QUERY with WIFI OFFER while it is on.
6. **Later:** upload GPS tracks and trip summaries.

**Data use:** with the access point off, about 1–2 MB per hour of driving (mostly street lookups) and almost nothing
parked.

**Throughput:** Cat-1 offers about 10 Mbit/s down. The UART link to the ESP32 is the real limit (a few Mbit/s at the
highest baud rates the module accepts). That is plenty for maps, weather, updates and telemetry, not for video.
