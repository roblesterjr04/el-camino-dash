# AM/FM radio card

**Status: spec only.** No schematic or board yet. When it is drawn, it follows the audio card's generator pattern
(`AudioCarrier/generate/`), and the GPS card's MCU and programming header.

**Unverified: the chip's pin names and timing below are from memory of the Silicon Labs Si4730/31/34/35-D60 data sheet
and AN332 / AN383.** The session that wrote this could not reach the data sheets (the network proxy blocked them).
Every item under [Check before drawing](#check-before-drawing) must be confirmed against the data sheets first.

A slot card for the DigiDash rev 2 board (`accessory_ports.md`, rev 2 section): an **AM/FM tuner** whose digital
audio goes to the audio card over the slot's **I2S link** (LINK 0–4), controlled by an **STM32G0B1** that also speaks
CAN. The audio card mixes the radio into the four outputs; nothing analog leaves this card.

```
                                  ┌────────────────────────── CAN: heartbeat, NET, UI (menu), SENSOR (signal), INPUT in,
 car antenna ─ SMA ─ matching ─ Si4735 ◄── I2C ── STM32G0B1 ──┤                 AUDIO_CMD (volume, sink status)
                       │  I2S out        ▲ RSTb, SEN, GPO     └── SWD, debug UART
                       ▼  (DOUT)          └── slot ID (pin 11)
        LINK 3 ◄─ 74LVC1G125 ─ 33 R ──┘
        LINK 0 (BCK), LINK 1 (WS) ───► Si4735 DCLK, DFS      (the audio card is the clock master)
```

## Where it goes

**Slot 2**, the link partner of the audio card in slot 1. It follows the audio card's clocks and sends its data on
LINK 3 (`can_channels.md`, [Links](../protocols/can_channels.md#links-between-slot-cards)): LINK 0 BCK in, LINK 1 WS in,
LINK 3 data out to the audio card's I2S1 data-in (GPIO34). LINK 2 (mic) and LINK 4 (MCLK) are not used.

> **Conflict to decide:** the AI voice card is also planned for slot 2. The audio card has one I2S1 input (GPIO34),
> and a pair has one partner, so **the radio and the voice card cannot both use the audio card's link.** Options: the
> radio takes slot 2 and the voice card waits; or the audio card's firmware mixes both onto LINK 3 by time-sharing
> (not designed); or the voice card moves to its own audio path later. Until decided, this spec assumes the radio gets
> the link.

Antenna and tuner parts are low and small. The card is 80 × 60 mm like every card, any parts up to 15 mm. It does
not need slot 1's open end.

## Parts

| Part | Choice | Why |
| --- | --- | --- |
| Tuner | **Si4735-D60** (AM / FM / SW / LW, RDS, 24-pin QFN) | One chip, I2C control, public programming guide (AN332), analog and **digital** audio out, no firmware to load. A-grade enough for a hobby dash. Not an automotive part. See [Why this tuner](#why-this-tuner) |
| MCU | **STM32G0B1KBU6** (as the GPS card) | FDCAN built in, 5 V tolerant I/O, I2C, plenty of flash for presets. Same SWD header and debug serial as the GPS card, so one programmer and one toolchain |
| CAN transceiver | MCP2562 (VDD 5 V, VIO 3.3 V, STBY low) | As every node. **No termination** |
| 3.3 V | AP2112K-3.3 from the slot's 5 V | MCU, transceiver VIO, tuner digital. The tuner's analog supply (VA) gets its own ferrite and 10 µF + 100 nF |
| Tuner reference clock | **32.768 kHz crystal on RCLK**, 12 pF loads | The tuner's standard clock. See the clock caveat under [Check before drawing](#check-before-drawing) |
| Antenna connector | **SMA, right angle, edge mount, on the top edge** (the case panel) | A 1964 car has a fixed whip with a Motorola plug: use a Motorola-to-SMA pigtail. A power antenna's motor lead is not on this card |
| Antenna network | Per Silicon Labs AN383 for a **whip**: FM through the matching network to FMI; AM through the short-wire network to AMI; ESD diode on the antenna pin, series L for the FM / AM split | Copy their schematic for the whip case. Never route either input near the buck, the crystal or the CAN pair |
| Link buffer | **74LVC1G125** on DOUT → LINK 3, enabled by an MCU pin | A card leaves its link pins high-Z until its partner has accepted the link (link rules). BCK / WS are inputs to the tuner and need no buffer, only the weak 100k pull-down |
| Link series R | 33 Ω on LINK 3 | As the audio card's |
| Programming (required: the STM32 is blank) | **J3 SWD, 1 × 5, 2.54 mm, right angle, left edge**: 3V3, SWDIO, SWCLK, NRST, GND | Exactly the GPS card's J3 (`gps_card.md`). Reachable with the card in its slot |
| Debug serial | **J4, 1 × 3, right angle**, beside J3: GND, TX, RX (USART1, 3.3 V) | Log output and the ROM bootloader fallback |
| Bench audio | 1 × 3 header, unfitted: LOUT, ROUT, GND | The tuner's analog outputs, to hear it on a bench with no audio card. Not connected to the slot |
| Indicators | One LED: off = tuner off, blinking = seeking, steady = tuned. One more: stereo / RDS | |

**Slot pins used:** 1 / 2 (+5 V), GND, 5 / 6 (CAN), 11 (slot ID), 15 / 16 (LINK 0 / 1, inputs), 18 (LINK 3, output).
Everything else is left unconnected, and the audio bus (21–23) is never driven.

## MCU pins (to fix in the schematic)

| Function | Notes |
| --- | --- |
| CAN | FDCAN1 TX / RX to the MCP2562 |
| Tuner I2C | I2C1 SCL / SDA, 4.7k pull-ups to 3.3 V. Tuner address chosen by its SEN pin (0x11 with SEN low, 0x63 with SEN high): strapped on the card |
| Tuner RSTb | GPIO output, open-drain style (low = reset) |
| Tuner GPO2 / INT | GPIO input: seek / tune complete and RDS interrupts, so the MCU does not poll |
| Link buffer enable | GPIO output, low (off) at reset, with a pull-up so the buffer is off until the firmware says so |
| Slot ID | An ADC input with 10k to 3.3 V (as every card) |
| LEDs | 2 GPIO |
| SWD / debug UART / HSE | As the GPS card |

## CAN

Node **`0x11`**, the first free accessory number (private range `0x440`–`0x47F`). Product **`0x0008`**, node type **9**
(radio / media source): both are new registry entries (see [Registry changes](#registry-changes)). The ANNOUNCE
carries the slot in its flags. The card implements the minimum level (heartbeat `0x711` with flags bit0 set, NET
ANNOUNCE / QUERY / IDENTIFY) and these channels:

| Channel | Role | What it does |
| --- | --- | --- |
| NET | both | ANNOUNCE (produces UI, SENSOR, consumes INPUT, AUDIO_CMD, SYSTEM). LINK: offers use 1 (I2S), **role 1 (follows)**, detail 4800 (48 kHz) to the audio card; drives LINK 3 only after the accept, and releases it on state 2 or when the audio card's heartbeat stops |
| UI | produces | The radio's menu page under **Menu → Accessories → Radio** (below) |
| SENSOR | produces | Sensor 0 signal strength (voltage quantity or unitless, dBµV, 1 s period), sensor 1 SNR (dB). Flag bit1 off: they are not gauges, only in the menu and the log |
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

The center shows what the **audio bridge** plays (`0x400`–`0x403`). The radio needs one of two things, a decision for
when firmware starts: the center learns to show a second source (radio) from the radio's UI texts, or the radio sends
the same transport / text frames in its private range (`0x440`, `0x441`) and the center is taught to read them when the radio is
the active source. Neither exists. Until then the radio's station and radio text are on its own menu page and the
console's Accessory page, which already show UI items.

### Source selection

There is no spec yet for choosing which source the audio card plays (phone via Bluetooth, the radio, the voice
card). **The audio card's firmware must grow a source select**, and the radio must send NET LINK only when it is the
selected source. A suggested first form: a UI choice on the audio card, `Source: Bluetooth / Radio`, and the
console's View or a long-press of play / pause cycling it. Not specified here.

## Board

- **80 × 60 mm**, 4 layers, any slot (it is meant for slot 2). Slot header at the bottom edge as on the audio card
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
| Documentation | AN332 / AN383, public | partial, project forks | NDA / limited |
| Grade | consumer | automotive | automotive |

The Si4735 is the one that can be built from public documents with no firmware blob. If the car's AM reception on a
whip is poor, the TEF6686 is the step up, with the firmware licence to sort out. The SMA, the I2S link and the MCU are
the same either way, so the card would only change the tuner and its matching.

## Check before drawing

1. **Which Si473x variant has the digital audio output.** The D60 data sheet's feature table lists it per part; the
   Si4735-D60 is the part assumed here. If it does not, the card needs an I2S ADC (PCM1808 as on the audio card) fed by
   LOUT / ROUT, in slave mode on the same LINK 0 / 1 and **a MCLK**, which would use LINK 4.
2. **Digital audio mode: slave or master.** The data sheet language remembered here is that the interface follows
   externally supplied DCLK / DFS (slave), which lets the audio card be the clock master as in the link rules. If the
   tuner is master only, the radio and the audio card would run on different clocks and the audio card would need a
   sample-rate converter. That is a stop-and-redesign item.
3. **Sample-rate lock.** In slave mode the tuner still makes its audio from its own reference. A 32.768 kHz crystal and
   the audio card's 48 kHz frame clock will drift, causing an occasional dropped or repeated sample. Test on the bench
   with a station and listen. The fix, if audible, is to run the tuner's RCLK from a clock the audio card also derives
   its frame from (RCLK needs 31.130 kHz–40 MHz in the reference modes; check which are allowed with digital out).
4. **Pin numbers and names** (DOUT, DFS, DCLK, RCLK, GPO1 / GPO2, SEN, RSTb, FMI, AMI) and the **power-up sequence**
   (`POWER_UP` command with the digital-output bit, `DIGITAL_OUTPUT_FORMAT`, `DIGITAL_OUTPUT_SAMPLE_RATE`).
5. **The whip antenna network** against AN383, for FM and AM. A car whip is tens of picofarads at FM and a lot more at
   AM; the AM short-wire input is not meant for a 1 m rod without the right network. If an amplified antenna is fitted
   in the car it needs a bias feed, which this card does not have.
6. **The SMA right-angle edge footprint** against the case panel hole, and the 2.5 mm back-side limit.
7. **I2S LINK levels:** the audio card's link buffer outputs 3.3 V, the tuner's VIO must be 3.3 V. The audio card's
   74LVC125 enable and the radio's 74LVC1G125 enable must agree with NET LINK (never both drive LINK 3).

## Firmware (STM32G0B1, Arduino STM32 core or Cube)

1. Boot: read the slot ID, bring the tuner out of reset (RSTb, SEN strapped), `POWER_UP` FM with digital output,
   set the I2S format (I2S, 16-bit) and sample rate 48 kHz, restore band / frequency / presets / region from flash,
   mute until the link exists.
2. CAN: heartbeat `0x711` (1 Hz), NET ANNOUNCE (with the slot), UI ITEMs at boot and on QUERY, SENSOR DEFINE.
3. LINK: when the audio card has announced and the radio is the source, send LINK offer (use 1, role 1, 4800); on
   accept, enable the link buffer and unmute. On release or the audio card's heartbeat stopping, mute and disable the
   buffer.
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
| Node numbers (`accessory_ports.md`) | `0x11` default for the radio (`0x440`–`0x47F`) |
