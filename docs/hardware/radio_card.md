# AM/FM radio card

**Status: spec only.** No schematic or board yet. When it is drawn, it follows the audio card's generator pattern
(`AudioCarrier/generate/`, `GPSCard/`, `LTECard/`), and the GPS card's MCU and programming header.

**Checked against the Si4730/31/34/35-D60 data sheet (Rev 1.1)**: pins, supplies, digital audio interface, reference
clock and package are as in this document. **Still unchecked** (needs AN332 and AN383, which are not in the repo): the
command and property details, the I2C address table, the list of supported RCLK frequencies, and the whip antenna network.
See [Check before drawing](#check-before-drawing).

A slot card for the DigiDash rev 2 board (`accessory_ports.md`, rev 2 section): an **AM/FM tuner** whose digital
audio goes to the audio card over the slots' **I2S rail**, controlled by an **STM32G0B1** that also speaks
CAN. The audio card mixes the radio into the four outputs; nothing analog leaves this card.

```
                                  ┌────────────────────────── CAN: heartbeat, NET, UI (menu), SENSOR (signal), INPUT in,
 car antenna ─ SMA ─ matching ─ Si4735 ◄── I2C ── STM32G0B1 ──┤                 AUDIO_CMD (volume, sink status)
                       │  I2S out        ▲ RSTb, SEN, GPO     └── SWD, debug UART
                       ▼  (DOUT)          └── slot ID (pin 11)
        RET (pin 18) ◄─ 74LVC1G125 ─ 33 R ──┘
        BCK (pin 15), WS (pin 16) ───► Si4735 DCLK, DFS      (the audio card is the clock master)
```

## Where it goes

**Any slot.** The slots share one I2S rail (`accessory_ports.md`, [I2S rail rules](accessory_ports.md#i2s-rail-rules)):
the audio card (slot 1) drives BCK (pin 15), WS (16), MIC (17) and MCLK (24); every other card is a slave. The radio
reads BCK and WS (pins 15 / 16) as the tuner's DCLK and DFS inputs, and sends its audio on **RET (pin 18)**, which
the audio card mixes into the four outputs. MIC and MCLK are not used.

RET has **one owner at a time**, granted by the audio card over CAN (NET RAIL, `can_channels.md`,
[The I2S rail](../protocols/can_channels.md#the-i2s-rail)). The radio asks for it when it becomes the active source,
drives RET only after the grant, and stops within 10 ms if the grant is withdrawn (a voice card talking outranks
music) or the audio card's heartbeat stops. A radio that is playing re-requests after a withdrawal ends. So the radio and
a voice card can share the rail without a slot pair; the voice card listens on MIC and takes RET only while it speaks.

Tall parts: none. The card is 80 × 60 mm like every card, parts up to 15 mm, and does not need slot 1's open end.

## Parts

| Part | Choice | Why |
| --- | --- | --- |
| Tuner | **Si4735-D60-GM** (AM / FM / SW / LW, RDS; 20-pin QFN, 3 × 3 mm, 0.5 mm pitch). **Not the Si4730**: it has no RDS processor. The Si4731-D60-GM is the AM/FM + RDS part if SW / LW are not wanted | One chip, I2C control, digital audio out (a slave interface: DCLK and DFS are inputs), no firmware to load. Not an automotive part. See [Why this tuner](#why-this-tuner) |
| MCU | **STM32G0B1KBU6** (as the GPS card) | FDCAN built in, 5 V tolerant I/O, I2C, plenty of flash for presets. Same SWD header and debug serial as the GPS card, so one programmer and one toolchain |
| CAN transceiver | MCP2562 (VDD 5 V, VIO 3.3 V, STBY low) | As every node. **No termination** |
| 3.3 V | AP2112K-3.3 from the slot's 5 V | MCU, transceiver VIO, and both tuner supplies: VA (2.7–5.5 V) and VD (1.62–3.6 V) both run at 3.3 V. The tuner draws about 10 mA on each in FM. VA gets its own ferrite and 22 nF (C1) close to the pin, VD 100 nF (C4) |
| Tuner reference clock | **A 32.768 kHz CMOS oscillator (±50 ppm or better, 1.8–3.3 V) into RCLK** | **The tuner's own crystal oscillator cannot be used**: it takes the GPO3 pin (pin 17), which is DCLK in digital-audio mode. So the clock comes from outside. ±100 ppm is the limit, ±50 ppm is needed for 50 kHz FM steps and for SW. See [Check before drawing](#check-before-drawing) |
| Antenna connector | **SMA, right angle, edge mount, on the top edge** (the case panel) | A 1964 car has a fixed whip with a Motorola plug: use a Motorola-to-SMA pigtail. A power antenna's motor lead is not on this card |
| Antenna network | **FM:** antenna → 1 nF (C2) → FMI (pin 2). **AM:** the data sheet's circuit is a ferrite loopstick (180–450 µH, L1) with 0.47 µF (C3) from AMI (pin 4), the other end on RFGND (pin 3); an air loop (10–20 µH) goes through a 1:5 transformer. **A car whip on AM is not in the data sheet**: AN383 has the network. Stray capacitance on the AM input must stay under 10 pF | The FM pin is 4 kΩ / 5 pF. Never route either input near the CAN pair, the oscillator or the MCU. ESD: these pins are under 2 kV HBM, so add a low-capacitance clamp on the SMA's centre |
| RET buffer | **74LVC1G125** on DOUT → RET, enabled by an MCU pin, pulled to "off" | Rail rule: every driver sits behind a 3-state buffer that is off at power-up. BCK / WS are inputs only and need no buffer |
| RET series R | 33 Ω on RET | As the rail rules ask |
| Programming (required: the STM32 is blank) | **J3 SWD, 1 × 5, 2.54 mm, right angle, left edge**: 3V3, SWDIO, SWCLK, NRST, GND | Exactly the GPS card's J3 (`gps_card.md`). Reachable with the card in its slot |
| Debug serial | **J4, 1 × 3, right angle**, beside J3: GND, TX, RX (USART1, 3.3 V) | Log output and the ROM bootloader fallback |
| Bench audio | 1 × 3 header, unfitted: LOUT, ROUT, GND (pins 14, 13) | The tuner's analog outputs, to hear it on a bench with no audio card (analog and digital out run together in OPMODE 0xB5). Not connected to the slot |
| Indicators | One LED: off = tuner off, blinking = seeking, steady = tuned. One more: stereo / RDS | |

**Slot pins used:** 1 / 2 (+5 V), GND, 5 / 6 (CAN), 11 (slot ID), 15 / 16 (I2S BCK / WS, inputs), 18 (I2S RET, output).
Everything else is left unconnected, and the audio bus (21–23) is never driven.

## MCU pins (to fix in the schematic)

| Function | Notes |
| --- | --- |
| CAN | FDCAN1 TX / RX to the MCP2562 |
| Tuner I2C | I2C1 SCL / SDA, 4.7k pull-ups to 3.3 V. The tuner's SEN pin (6) sets its address in 2-wire mode (0x11 low / 0x63 high from AN332, to confirm): strapped on the card |
| Tuner RSTb | GPIO output (low = reset). **Bus mode is latched at the rising edge**: GPO1 must be high and GPO2 low (their internal pull-up / pull-down do it: leave both unconnected or high-Z), and SCLK high at that edge, SDIO not falling within 300 ns before it. RSTb low at least 100 µs |
| Tuner GPO2 / INT (pin 18) | GPIO input: seek / tune complete (STC) and RDS interrupts. The data sheet asks for a quiet I2C bus during a seek or tune (crystal jitter, false stops), so the MCU waits for the interrupt and does not poll |
| RET buffer enable | GPIO output, low (off) at reset, with a pull-down so the buffer is off until the firmware says so |
| Slot ID | An ADC input with 10k to 3.3 V (as every card) |
| LEDs | 2 GPIO |
| SWD / debug UART / HSE | As the GPS card |

## CAN

Node **`0x14 + (slot − 1)`**, read from the slot ID pin at boot (`accessory_ports.md`, Node numbers); the card has no
number of its own, and stays off the bus if it reads no valid slot. Product **`0x0008`**, node type **9** (radio /
media source): both are new registry entries (see [Registry changes](#registry-changes)). The ANNOUNCE carries the
slot in its flags. The card implements the minimum level (heartbeat `0x700 + node` with flags bit0 set, NET ANNOUNCE /
QUERY / IDENTIFY) and these channels:

| Channel | Role | What it does |
| --- | --- | --- |
| NET | both | ANNOUNCE (produces UI, SENSOR, consumes INPUT, AUDIO_CMD, SYSTEM). RAIL: sends state 0 (request) to the audio card (found from its ANNOUNCE: node type 5, product `0x0004`) with the sample rate it can follow (48 kHz); enables the RET buffer only on state 1 (granted); releases (state 2) when it stops being the source, the user turns it off, or on RESTART |
| UI | produces | The radio's menu page under **Menu → Accessories → Radio** (below) |
| SENSOR | produces | Sensor 0 signal strength (dBµV, 1 s period), sensor 1 SNR (dB). Flag bit1 off: they are not gauges, only in the menu and the log |
| INPUT | consumes | With the radio the active source: key 17 / 18 (next / previous) seek, 16 play / pause = mute the radio, encoder 1 is left to the audio sink. When another source is active the radio ignores them |
| AUDIO_CMD | consumes | Reads the sink's STATUS: muted, music playing / ducked. It pauses itself on mute (sink says muted) |
| SYSTEM | consumes | POWER (ignition off): save the last station and mute. TIME is not needed |
| LOG | produces | Lines at level info for tune and seek results |

### UI items

| Item | Kind | Notes |
| --- | --- | --- |
| Band | choice | FM, AM |
| Frequency | number | FM 87.5–108.0 MHz in 0.1 MHz steps (exponent −1, MHz); AM 520–1710 kHz in 10 kHz steps. Setting it tunes |
| Seek up / Seek down | action | Seek to the next station that passes the thresholds; the new frequency comes back as a VALUE |
| Station | text readout | RDS program-service name (8 characters) |
| Radio text | text readout | RDS radiotext, shortened to 64 bytes |
| Stereo | readout | 0 mono / 1 stereo |
| Preset 1–12 | choice + action | Store the current station / recall. Items are flagged bit4 so the center backs them up |
| Seek threshold | number | RSSI and SNR limits, **advanced** flag |
| Region | choice | US (100 kHz FM grid, 75 µs de-emphasis, AM 10 kHz) / Europe (50 / 100 kHz, 50 µs, AM 9 kHz), flag bit4 |
| Radio on | toggle | Whether the tuner is powered; **off** powers the tuner down and releases the link |

The radio keeps its own state (band, frequency, presets, region) in the MCU's flash and works without the center.
A SET from the menu is applied, saved and answered with VALUE and RESULT, as for every UI item.

### Now playing on the media screen

The center shows what the **audio bridge** plays (`0x400`–`0x403`, node `0x10`; the slot-card audio card's number is
`0x14`, slot 1). The radio needs one of two things, a decision for when firmware starts: the center learns to show a
second source (radio) from the radio's UI texts, or the radio sends the same transport / text frames in its private range
(`0x400 + 0x40·(node − 0x10)`) and the center is taught to read them when the radio is the active source. Neither exists. Until then the radio's station and radio text are on its own menu page and the
console's Accessory page, which already show UI items.

### Source selection

There is no spec yet for choosing which source the audio card plays (phone via Bluetooth, the radio, the voice
card). **The audio card's firmware must grow a source select**, and the radio must request RET only when it is the
selected source. A suggested first form: a UI choice on the audio card, `Source: Bluetooth / Radio`, and the
console's View or a long-press of play / pause cycling it. Not specified here.

## Board

- **80 × 60 mm**, 4 layers, any slot. Slot header at the bottom edge as on the audio card
  (`AudioCarrier/generate/gen_pcb.py` has the orientation check).
- **Top edge** (the case panel): the SMA jack. The RF part (antenna, matching, tuner) sits close under it, on the
  **parts side**, with a **solid ground under it (In1)** and no digital traces across it. Keep the tuner's antenna
  inputs away from the noisy side: the MCU, CAN transceiver and the slot header go at the bottom.
- **Left edge:** J3 (SWD) and J4 (debug serial), right angle.
- **Back side:** only low SMD parts (≤ 2.5 mm including solder tails).
- **Crystal** next to the tuner's RCLK pin with ground around it. The 32.768 kHz is weak: keep it short.
- **Shield:** leave room for an optional tin shield can over the tuner and its front end.

## Why this tuner

| | Si4735-D60 | TEF6686 | Si4684 / Si4688 |
| --- | --- | --- | --- |
| Bands | AM, FM, SW, LW | AM, FM (better) | AM, FM, DAB |
| Control | I2C, no firmware load | I2C, **needs a firmware patch** at every boot, from NXP under licence | I2C / SPI, **needs external flash and a firmware image** |
| Digital audio | I2S (check variant) | I2S | I2S |
| Documentation | data sheet, AN332 / AN383 | partial, project forks | NDA / limited |
| Grade | consumer | automotive | automotive |

The Si4735 is the one that can be built from public documents with no firmware blob. If the car's AM reception on a
whip is poor, the TEF6686 is the step up, with the firmware licence to sort out. The SMA, the I2S link and the MCU are
the same either way, so the card would only change the tuner and its matching.

## Si4735 hookup (from the data sheet)

QFN-20 (GM), pins used. 3.3 V on VA and VD.

| Pin | Name | Goes to |
| --- | --- | --- |
| 2 | FMI | FM antenna through 1 nF |
| 3 | RFGND | AM antenna return, to ground plane |
| 4 | AMI | AM antenna through 0.47 µF |
| 5 | RST | MCU (active low) |
| 6 | SEN | strap (address) |
| 7 | SCLK | MCU I2C SCL |
| 8 | SDIO | MCU I2C SDA |
| 9 | RCLK | 32.768 kHz oscillator |
| 10 | VD | 3.3 V, 100 nF |
| 11 | VA | 3.3 V (ferrite), 22 nF |
| 12, pad | GND | ground |
| 13, 14 | ROUT, LOUT | bench header (OPMODE 0xB5) |
| 15 | DOUT | RET buffer through 600 Ω (R1) |
| 16 | DFS | I2S WS (pin 16), through 2 kΩ (R2) |
| 17 | GPO3 / DCLK | I2S BCK (pin 15), through 2 kΩ (R3) |
| 18 | GPO2 / INT | MCU |
| 19 | GPO1 | MCU (spare), must be high or open at reset |
| 1, 20 | NC | leave floating |

The 600 Ω / 2 kΩ / 2 kΩ series resistors and a 2–5 pF noise capacitor (C9) are the data sheet's optional digital-audio
network, there to keep the digital edges out of the RF. They go on the card. DCLK min cycle 26 ns (the rail's 3.07 MHz
BCK is 325 ns), DFS set-up and hold 5 ns, DOUT valid within 50 ns of DCLK falling: no problem at the rail's 64 fS.
I2S, left-justified and DSP formats, 8 to 24 bits, 32 / 44.1 / 48 kHz. **I2S mode, default MSB on the second DCLK edge
after the DFS edge: matches the rail.** DFS low = left.

## Check before drawing

Settled by the data sheet: the digital audio interface is a **slave** (DCLK and DFS are inputs), so the audio card is the
only clock master as the rail rules want; every variant has digital audio out; the pins in the table above.

1. **The reference clock** (found by the data sheet): the tuner's crystal oscillator and the digital audio output
   both use GPO3 (pin 17), so a crystal is out and **RCLK needs an oscillator**. Which RCLK frequencies are accepted is
   AN332 Table 6 (31.130 kHz–40 MHz with a prescaler, but "some frequencies are not supported"). If the audio card's
   12.288 MHz MCLK (rail pin 24) is accepted, the tuner would run on the audio card's sample clock and sample-rate drift
   would not exist; but MCLK's harmonics fall in the FM band (the 8th is 98.3 MHz). If it is not accepted, a 32.768 kHz
   oscillator is used and the sample clocks are asynchronous (point 2).
2. **Sample-rate lock.** DFS comes from the audio card, but the tuner makes its audio from its own reference clock. How the
   chip behaves when the two differ (it may slip a sample every so often, or follow DFS) is not in this data sheet: AN332's
   DIGITAL_OUTPUT_SAMPLE_RATE property says. Listen to it on the bench with a station; a click every few seconds means
   the clocks must be tied together.
3. **AN332:** POWER_UP (the OPMODE byte, CTS and the 110 ms power-up), DIGITAL_OUTPUT_FORMAT (I2S, 16 bit, DFS / DCLK
   edges) and DIGITAL_OUTPUT_SAMPLE_RATE, RX_VOLUME, FM_RDS_* properties, the seek and tune thresholds, the I2C address
   by SEN, and whether DCLK must be running when digital output is turned on.
4. **AN383 for the whip.** A car whip is a 4–5 pF-ish source at FM and hundreds of pF at AM; the AM input wants a high
   impedance and under 10 pF stray, so the network for a whip needs the application note. If the car's antenna is amplified, it needs a bias feed
   this card does not have.
5. **The SMA right-angle edge-mount footprint** (part chosen, drawing in hand), against the case panel hole and the 2.5 mm
   back-side limit.
6. **RET enable** must follow NET RAIL exactly (never drive RET without a grant). With the rail idle (no clocks) the
   tuner's digital output must not be enabled (no DCLK).
7. **ESD:** some tuner pins are rated under 2 kV HBM; treat it as ESD-sensitive at assembly and keep the clamp on the antenna pin.

## Firmware (STM32G0B1, Arduino STM32 core or Cube)

1. Boot: read the slot ID, bring the tuner out of reset (RSTb, SEN strapped, SCLK high, GPO1 / GPO2 at their default levels at the rising edge), `POWER_UP` FM with digital output (OPMODE 0xB0, or 0xB5 with the bench outputs),
   set the I2S format (I2S, 16-bit) and sample rate 48 kHz, restore band / frequency / presets / region from flash,
   mute until the link exists.
2. CAN: heartbeat `0x700 + node` (1 Hz), NET ANNOUNCE (with the slot), UI ITEMs at boot and on QUERY, SENSOR DEFINE.
3. RAIL: when the audio card has announced and the radio is the source, send RAIL state 0; on state 1, enable the
   RET buffer and unmute. On state 2 (withdrawn), or the audio card's heartbeat stopping, mute and disable the buffer
   within 10 ms; ask again once it is free.
4. Radio loop: GPO2 interrupt for seek / tune complete and RDS; read RSSI / SNR at 1 Hz for SENSOR; assemble RDS PS and
   radiotext (group 0A / 2A) and send UI TEXT on change; save the station (debounced) to flash.
5. LOG: tune and seek results at info level.

Host-testable parts (CAN codecs, RDS group assembly, preset storage) go in `firmware/elc_can` and a new
`firmware/radio_card` directory with `test/host/` like the others. `firmware/elc_can` has no NET, UI or SENSOR codecs
yet (`can_channels.md` is spec only), so this card's firmware waits on step 1 of the channel protocol's order of work.

## Registry changes

Append-only changes this card needs in `can_channels.md`:

| Registry | Entry |
| --- | --- |
| Products | `0x0008`: AM/FM radio card |
| Node types (NET text) | 9 radio / media source |
