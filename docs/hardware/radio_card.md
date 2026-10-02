# AM/FM radio card

**Status: schematic and board done in [`RadioCard/`](../../RadioCard/README.md)** (80 × 60 mm, 4 layers, placed and
routed). It follows the audio card's generator pattern (`AudioCarrier/generate/`, `GPSCard/`, `LTECard/`), and the GPS
card's MCU and programming header. Until the card is built, `tools/can_relay.py` stands in for it on the bench
(`tools/radio_sim.py`, see [Simulated card](#simulated-card)).

**Checked against** the Si4730/31/34/35-D60 data sheet (rev 1.2), AN332 *Si47xx Programming Guide* (Skyworks 207003A),
AN383 *Si47xx Antenna, Schematic, Layout and Design Guidelines* (207075A) and ST DS13560 rev 4. What they settled is in
[Settled by the documents](#settled-by-the-documents); what is still open is in [Open](#open).

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
| Tuner | **Si4735-D60-GU** (AM / FM / SW / LW, RDS; **SSOP-24**, hand-solderable; the -GM QFN-20 is the same silicon, but its land pattern is not a stock KiCad footprint). **Not the Si4730**: it has no RDS processor. The Si4731-D60-GM is the AM/FM + RDS part if SW / LW are not wanted | One chip, I2C control, digital audio out (a slave interface: DCLK and DFS are inputs), no firmware to load. Not an automotive part. See [Why this tuner](#why-this-tuner) |
| MCU | **STM32G0B1KBU6** (as the GPS card) | FDCAN built in, 5 V tolerant I/O, I2C, plenty of flash for presets. Same SWD header and debug serial as the GPS card, so one programmer and one toolchain |
| CAN transceiver | MCP2562 (VDD 5 V, VIO 3.3 V, STBY low) | As every node. **No termination** |
| 3.3 V | AP2112K-3.3 from the slot's 5 V | MCU, transceiver VIO, and both tuner supplies: VA (2.7–5.5 V) and VD (1.62–3.6 V) both run at 3.3 V. The tuner draws about 10 mA on each in FM. VA gets its own ferrite and 22 nF (C1) close to the pin, VD 100 nF (C4) |
| Tuner reference clock | **None on the card: the tuner takes its reference from DCLK** (the rail's BCK, 3.072 MHz at 48 kHz): `REFCLK_PRESCALE` = `0x1060` (bit 12 RCLKSEL = DCLK, prescale 96), `REFCLK_FREQ` = 32000 (AN332, properties `0x0201` / `0x0202`, p. 37–38) | The tuner's own crystal oscillator cannot be used (it shares GPO3, which is DCLK in digital-audio mode). Taking the reference from the audio card's bit clock means the tuner runs on the audio card's sample clock: **no drift between the two**, and no oscillator to buy. The BCK comes from the audio card's crystal, well inside the ±100 ppm limit. Fallback, not fitted: a 0 Ω (R13) from the rail's MCLK (12.288 MHz, prescale 375 = 32768 Hz) to RCLK, with RCLKSEL = RCLK; MCLK's 8th harmonic is 98.3 MHz. RCLK has a 100k pull-down when unused |
| Antenna connector | **Amphenol RF 132289, SMA jack, edge mount for a 1.6 mm board, on the top edge** (the case panel) | A 1964 car has a fixed whip with a Motorola plug (DIN 41585 male): use a short pigtail, DIN 41585 female to SMA male (not RP-SMA), e.g. Eightwood TB0188 (10 cm RG174). No straight adapter is sold, and no PCB-mount DIN 41585 jack with a published drawing was found. A power antenna's motor lead is not on this card |
| Antenna network | **FM** (AN383 §7, fig. 23): the antenna node has **L1 56 nH** (Murata LQW18AN56NJ00D, Q ≥ 15 at 100 MHz) to ground, then **1 nF C0G** to FMI. **AM** (data sheet, AN383 §8): a ferrite loopstick (180–450 µH) on **J5** between the AM node and ground, **0.47 µF** to AMI. **A car whip on AM is in neither document** (AN383 §10's SW-whip network is the nearest): JP2 (open) is where it would join. AM stray capacitance under 10 pF | L1 matches a ~22 pF whip; a car whip on a metre of cable is more, so **tune L1 on the bench** (or leave it off). Never route either input near the CAN pair or the MCU. FMI / AMI absolute maximum 0.4 Vpk |
| Antenna ESD | **TI TPD1E0B04DPYR** (0.13 pF, X1SON 1.0 × 0.6 mm) on the SMA centre | AN383 asks for a clamp of ≤ 1 pF wherever the antenna is exposed (it names the CM1213-01ST) |
| RET buffer | **74LVC1G125** on DOUT → RET, enabled by an MCU pin, pulled to "off" | Rail rule: every driver sits behind a 3-state buffer that is off at power-up. BCK / WS are inputs only and need no buffer |
| RET series R | 33 Ω on RET | As the rail rules ask |
| Programming (required: the STM32 is blank) | **J3 SWD, 1 × 5, 2.54 mm, right angle, left edge**: 3V3, SWDIO, SWCLK, NRST, GND | Exactly the GPS card's J3 (`gps_card.md`). Reachable with the card in its slot |
| Debug serial | **J4, 1 × 3, right angle**, beside J3: GND, TX, RX (USART1, 3.3 V) | Log output and the ROM bootloader fallback |
| Bench audio | **None** | The draft had an analog header, but AN332 (p. 15) offers analog + digital out together (OPMODE `0xB5`) on the Si4735 only for AM; FM is digital only (`0xB0`). LOUT / ROUT are left open |
| Indicators | One LED: off = tuner off, blinking = seeking, steady = tuned. One more: stereo / RDS | |

**Slot pins used:** 1 / 2 (+5 V), GND, 5 / 6 (CAN), 11 (slot ID), 15 / 16 (I2S BCK / WS, inputs), 18 (I2S RET, output),
24 (MCLK, only through the unfitted R13).
Everything else is left unconnected, and the audio bus (21–23) is never driven.

## MCU pins (to fix in the schematic)

| Function | Notes |
| --- | --- |
| CAN | FDCAN1 on PA12 / PA11 (pins 23 / 22), **AF3** (DS13560 table 15), to the MCP2562 |
| Tuner I2C | I2C1 on PB6 / PB7 (pins 30 / 31, AF6), 4.7k pull-ups to 3.3 V. The tuner's SEN pin sets its 7-bit address: **0x11** low (JP1 as delivered), 0x63 high (AN332 §6.1). At most 8 bytes per write, 16 per read |
| Tuner RSTb | GPIO output (low = reset). **Bus mode is latched at the rising edge**: GPO1 must be high and GPO2 low (their internal pull-up / pull-down do it: leave both unconnected or high-Z), and SCLK high at that edge, SDIO not falling within 300 ns before it. RSTb low at least 100 µs |
| Tuner GPO2 / INT (pin 18) | GPIO input: seek / tune complete (STC) and RDS interrupts. The data sheet asks for a quiet I2C bus during a seek or tune (reference jitter, false stops), so the MCU waits for the interrupt and does not poll |
| RET buffer enable | GPIO output, low (off) at reset, with a pull-down so the buffer is off until the firmware says so |
| Slot ID | An ADC input with 10k to 3.3 V (as every card) |
| LEDs | 2 GPIO |
| SWD / debug UART / HSE | As the GPS card |

## CAN

Node **`0x14 + (slot − 1)`**, read from the slot ID pin at boot (`accessory_ports.md`, Node numbers); the card has no
number of its own, and stays off the bus if it reads no valid slot. Product **`0x0009`**, node type **9** (radio /
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
- **Top edge** (the case panel): the SMA jack J2 (x 45) and the AM loopstick header J5 (x 33, right angle). The RF
  part (ESD clamp, L1, the coupling caps, the tuner) sits close under them on the **parts side**, with the **solid In1
  ground plane under it** and a keep-out on In2 and B (no tracks; ground stitching vias only) from x 28 to 62, y 0 to
  21. The tuner is turned so FMI / AMI face the SMA. The MCU, CAN transceiver, RET buffer and the slot header are at
  the bottom.
- **Left edge:** J3 (SWD) and J4 (debug serial), right angle.
- **Back side:** empty: every part is on the parts side.
- **No crystal or oscillator for the tuner** (its reference is DCLK). The STM32's 8 MHz crystal is beside the MCU, far
  from the tuner.
- **Shield:** none on rev A; the keep-out leaves room to add a tin shield can over the tuner later.

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

SSOP-24 (GU), pins used. 3.3 V on VA and VD. (QFN-20 pin numbers differ; the data sheet's figure is the reference.)

| Pin | Name | Goes to |
| --- | --- | --- |
| 1 | DOUT | RET buffer through 604 Ω (R1; data sheet 600 Ω, AN383 604 Ω) |
| 2 | DFS | I2S WS (slot pin 16), through 2 kΩ (R2) |
| 3 | GPO3 / DCLK | I2S BCK (slot pin 15), through 2 kΩ (R3) |
| 4 | GPO2 / INT | MCU |
| 5 | GPO1 | left open: must be high or open at reset |
| 6, 7 | NC | leave floating |
| 8 | FMI | FM antenna through 1 nF (56 nH to ground at the antenna node) |
| 9 | RFGND | ground |
| 10, 11 | NC | tie to GND (SSOP only) |
| 12 | AMI | AM antenna through 0.47 µF |
| 13, 14 | GND | ground |
| 15 | RST | MCU (active low) |
| 16 | SEN | strap (address) |
| 17 | SCLK | MCU I2C SCL |
| 18 | SDIO | MCU I2C SDA |
| 19 | RCLK | 100k to GND; R13 (not fitted) from the rail MCLK |
| 20 | VD | 3.3 V, 100 nF |
| 21 | VA | 3.3 V through a ferrite |
| 22 | DBYP | 22 nF to VA (C1, as in the data sheet's SSOP schematic, p. 18) |
| 23, 24 | ROUT, LOUT | open (FM digital out only) |

The 604 Ω / 2 kΩ / 2 kΩ series resistors and a 2.2 pF C0G noise capacitor (C9) are the data sheet's optional digital-audio
network, there to keep the digital edges out of the RF. They go on the card. DCLK min cycle 26 ns (the rail's 3.07 MHz
BCK is 325 ns), DFS set-up and hold 5 ns, DOUT valid within 50 ns of DCLK falling: no problem at the rail's 64 fS.
I2S, left-justified and DSP formats, 8 to 24 bits, 32 / 44.1 / 48 kHz. **I2S mode, default MSB on the second DCLK edge
after the DFS edge: matches the rail.** DFS low = left.

## Settled by the documents

- **Digital audio is a slave interface** (DCLK, DFS inputs), so the audio card stays the only clock master. I2S,
  16-bit, rising edge: `DIGITAL_OUTPUT_FORMAT` = `0x0000`. `DIGITAL_OUTPUT_SAMPLE_RATE` is the DFS rate (48000);
  DCLK must be at least 1 MHz (3.072 MHz here). **DCLK and DFS must be running before the sample rate is set**, or
  the chip stops answering until reset; **set the rate to 0 before the clocks stop** (AN332 p. 36). `FM_TUNE_FREQ` comes
  after POWER_UP and before the sample rate.
- **Reference clock from DCLK** (RCLKSEL), see [Parts](#parts). REFCLK must be valid while tuning and for AFC, so
  the radio tunes only while the rail has clocks.
- **POWER_UP**: `01 C0 B0` for FM (CTS and GPO2 interrupts, XOSCEN = 0, OPMODE `0xB0` digital), `01 C1 B0` for AM
  (AN332 p. 14–15, 72–73). XOSCEN must be 0: there is no crystal.
- **I2C**: 2-wire mode by GPO1 high and GPO2 low at the RST rising edge, with SCLK high; address 0x11 (SEN low).
- **MCU pins** (DS13560 rev 4): the KBU6 is the "GP" UFQFPN-32 pinout; FDCAN1 is AF3 on PA11 / PA12, I2C1 AF6 on PB6 / PB7,
  USART1 AF1 on PA9 / PA10, no bonded pins.
- **RCLK** must not be driven while VD is off (absolute maximum VD + 0.3 V): R13's source, the rail MCLK, only runs
  while the slot's 5 V is up, and the card's 3.3 V follows it within the regulator's start-up.

## Open

1. **AM from the car whip.** Neither the data sheet nor AN383 has a network for a car whip on AMI. The card ships with
   AM from a loopstick on J5; JP2 joins the SMA to the AM node once a network is worked out (AN383 §10's SW-whip
   circuit is the starting point). If the car's antenna is amplified, it needs a bias feed this card does not have.
2. **L1 on the car's whip.** 56 nH matches AN383's ~22 pF whip. Check FM reception on the bench with the real whip and
   cable, with and without L1.
3. **Land patterns to check against the parts in hand:** the SMA (Amphenol 132289) on the top edge, and D2's 0402
   pattern against TI's DPY drawing.
4. **The BCK as a reference while the audio card changes rate.** At 44.1 kHz the BCK is 2.8224 MHz: prescale 86 gives
   32 818 Hz (in range). The radio must re-set `REFCLK_PRESCALE` / `REFCLK_FREQ` (only possible in powerup) or ask for
   48 kHz in its RAIL request, as it does.

## Firmware (STM32G0B1, Arduino STM32 core or Cube)

1. Boot: read the slot ID, restore band / frequency / presets / region from flash. Wait for the rail's clocks (the audio
   card's heartbeat and announce). Then bring the tuner out of reset (RSTb low ≥ 100 µs, SCLK high, GPO1 / GPO2 at
   their default levels at the rising edge), `POWER_UP` `01 C0 B0`, `REFCLK_PRESCALE` `0x1060`, `REFCLK_FREQ` 32000,
   `DIGITAL_OUTPUT_FORMAT` 0, `FM_TUNE_FREQ`, then `DIGITAL_OUTPUT_SAMPLE_RATE` 48000. Muted until the link exists.
   Before the audio card stops (its heartbeat goes, SYSTEM POWER), set the sample rate to 0.
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

## Simulated card

`tools/can_relay.py` runs `tools/radio_sim.py`, which sends what this firmware will send, from slot 2 (node `0x15`)
unless told otherwise: the heartbeat, ANNOUNCE (product `0x0009`, node type 9), the UI page below, SENSOR 0 / 1, LOG
lines, and RAIL requests once an audio card (node type 5, product `0x0004`) has announced and the radio is the
selected source. It answers QUERY, SET, VIEW, RAIL, IDENTIFY, RESTART, the keys and SYSTEM POWER. The stations are made
up. Its item numbers are the ones the firmware should use:

| Item | Label | Kind | Notes |
| --- | --- | --- | --- |
| 0 | Radio on | toggle | setting |
| 1 | Band | choice | FM, AM; setting |
| 2 | Frequency | number | FM: × 0.1 MHz, 875–1080; AM: kHz, 520–1710 step 10 (Europe 522–1710 step 9). Its ITEM is resent on a band change |
| 3, 4 | Seek up, Seek down | action | |
| 5, 6 | Station, Radio text | text readout | RDS PS and radiotext |
| 7, 8 | Stereo, Signal | readout | live |
| 9 | Preset | choice | recall: the options list the 12 presets ("1  101.1 FM", "2  --") |
| 10 | Store as preset | choice | asks to confirm; stores the current station in the chosen slot |
| 11 | Region | choice | US, Europe; setting |
| 12, 13 | Seek signal min, Seek SNR min | number | advanced, setting |
| 14 | Preset memory | group | advanced: items 15–26, each preset's frequency in kHz (0 = empty), as settings so the center backs them up. The console's preset list reads these |

SENSOR 0 "Radio signal dBuV" and 1 "Radio SNR dB" use quantity 0 (unitless): the registry has no dBµV or dB.

## Registry changes

Append-only changes this card needs in `can_channels.md`:

| Registry | Entry |
| --- | --- |
| Products | `0x0009`: AM/FM radio card |
| Node types (NET text) | 9 radio / media source |
