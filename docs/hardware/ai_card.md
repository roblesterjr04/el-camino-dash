# AI voice card

**Status: requirements, firmware and a first board design (placed and routed, not ordered; the compute module's size and pin map are provisional).** Firmware: [`firmware/ai_card/`](../../firmware/ai_card/README.md) (the ESP32-S3
front end) and [`firmware/ai_agent/`](../../firmware/ai_agent/README.md) (the assistant that runs on the compute module).
Board: [`AICard/`](../../AICard/README.md). Link between the two processors: [`docs/protocols/ai_link.md`](../protocols/ai_link.md).

A slot card (`docs/hardware/accessory_ports.md`, rev 2) that gives the dash a voice assistant. You say the wake word (or
press a key), it hears you through the audio card's microphone, answers through the audio card's speakers, and can
read the car's own sensors, run the media, and raise alerts. **It works with no internet** (an on-board speech
recogniser, language model and voice), and **uses the internet when there is one** for facts the car cannot know
(news, businesses, directions, "what year did…"), falling back to the on-board model when the connection is slow or gone.

Registry entries already exist for it: node type 8 (voice assistant), product `0x0007` (`can_channels.md`).

## What it does

| # | Requirement | Notes |
| --- | --- | --- |
| F1 | Wake by **wake word** (always listening, on the card, no cloud) or by **key** (INPUT `F1`, e.g. a console key or a steering-wheel button) | The wake word never leaves the ESP32-S3 |
| F2 | **Speech to text on the card.** Audio never leaves the car | whisper.cpp (`base.en` / `small.en`) or sherpa-onnx, on the compute module |
| F3 | **Answer car questions from live data**: oil pressure, temperature, fuel, speed, range, MPG, alerts, odometer; plus place, weather and time from the LTE gateway when there is one | Deterministic, no model needed, always works |
| F4 | **Control the dash by voice**: volume, mute, next / previous, source (Bluetooth / radio / off), tune, presets | Over the channels (AUDIO_CMD, INPUT). Allow-listed on the ESP32 (below) |
| F5 | **Offline free-form Q&A** with a small local language model, grounded with the live sensor data and (optionally) the owner's manual on the card | llama.cpp, 1–3 B parameters, 4-bit |
| F6 | **Online lookups when an uplink exists**: a larger model with web search, used for questions that need fresh facts | Claude through the `anthropic` SDK with its web-search tool; any OpenAI-compatible server as an alternative. **Only the question text and a small context summary go out, never audio** |
| F7 | **Automatic fallback**: online first when it is up and the question needs it; the local model answers when the network is down, slow (> 4 s to first word) or refuses | The driver can force *Offline only* in the menu |
| F8 | **Never touches the car.** The assistant can speak, show text, change media and raise an *info / advisory* alert. It cannot send SYSTEM, sensor, calibration or any alert above *advisory*; the ESP32 enforces that, not the AI | Safety rule: the model is untrusted input |
| F9 | **Speak the answer** through the audio card (RET), ducking the music, and also show it as text (UI page, toast) so a missed answer is not lost | Local TTS (piper) |
| F10 | **Settings on the dash menu** (UI channel): enabled, wake word on / off, online lookups auto / off / always, volume of the voice, speak answers, mute microphone, a "forget the conversation" button | Backed up by the center like any accessory |
| F11 | **Privacy**: a hard microphone-mute toggle (the ESP32 stops listening), an LED while the microphone is open, no audio stored, conversation history kept in RAM only | |

## Architecture

```
                         slot 1 (audio card)                                   this card
 microphone ─ PCM1808 ─ rail MIC / BCK / WS ─────────────────────────────► ESP32-S3 (always on, from slot +5 V)
 speakers  ◄─ PCM5102A ◄─ audio card mixes (ducks music) ◄─ rail RET ◄───────  ├─ wake word, key, offline commands
                                                                              ├─ CAN node 0x14 + slot − 1 (node type 8, product 0x0007)
 other cards / center ◄═══ CAN (500 kbit/s) ═══════════════════════════════►  ├─ allow-list + rate limit for what the AI may send
                                                                              ├─ power sequencing of the compute module
                                                                              └─ UART 1.5 Mbaud (framed, CRC) ══╗
                                                                                                                 ║
 AUX 12 V ─ buck ─ 5 V ─ (switched by the ESP32) ──────────────────────────► compute module (Linux, 8 GB, 40-pin header)
                                                                              ├─ speech to text    (offline)
                                                                              ├─ intent engine     (offline, deterministic)
                                                                              ├─ local LLM + manual search (offline)
                                                                              ├─ text to speech    (offline)
                                                                              └─ online provider   (Wi-Fi to the phone's hotspot, or a gateway)
```

Why two processors:

- **The ESP32-S3 owns everything that must be there at key-on and must never misbehave:** CAN (heartbeat, ANNOUNCE,
  menu), the I2S rail drivers (3-state, off at power-up, as the rail rules require), the wake word, the power switch for
  the big module, and the rules for what the AI may do. It runs from the slot's 5 V and is the only part with a
  guaranteed state.
- **The compute module is a Linux computer** that takes 20–40 s to boot, draws watts, gets hot and can crash. It is
  switched from the ESP32, shut down cleanly on key-off, power-cycled by the ESP32 if it hangs, and treated as untrusted.

The ESP32 and the module see each other only through one UART with framing and a CRC (`ai_link.md`). The module does not
see CAN directly: the ESP32 forwards the channel messages the assistant needs and sends the ones it is allowed to.

## Compute module requirements

The module is a **ready-made board on a 2×20 (Raspberry-Pi-style) header**, in the same way the LTE card carries a
ready-made core board on sockets: the 40-pin pinout is a standard, so the card does not depend on one vendor.

| Requirement | Minimum | Recommended | Why |
| --- | --- | --- | --- |
| CPU | 4 × Cortex-A55 @ 1.8 GHz | 4 × Cortex-A76 (Pi 5 class) or better | Prompt processing for the LLM, and speech recognition in about real time |
| RAM | 4 GB | **8 GB** | Local model 1–2.5 GB + recogniser 0.3 GB + voice 0.1 GB + OS and page cache. 8 GB lets the model stay resident and the manual's index load |
| Storage | 16 GB eMMC | 32–64 GB eMMC | Models, the manual, logs. **eMMC, not a microSD card**: a dash vibrates and a card pops out; power loss and wear are also kinder on eMMC with a read-only root |
| Wi-Fi | 802.11n | Wi-Fi 5 / 6 | The uplink to a phone hotspot. BT is not used |
| Header | 40-pin, Raspberry-Pi-compatible pinout, **5 V on pins 2 / 4**, UART on pins 8 / 10 | | Power, link, three control lines |
| Supply | 5 V, **3 A peak** | | The card budgets 3 A |
| Operating temperature | 0–70 °C at the module | −20…85 °C | A dash cavity in summer. With the heatsink the card throttles before it overheats (below) |
| Size | ≤ 65 × 30 mm | | Stacks over the card on the 2×20 socket. 65 × 30 mm is the Pi Zero / Radxa Zero size |
| Software | Mainline-ish Linux, a vendor image with working Wi-Fi and a serial console, `llama.cpp` and `whisper.cpp` build | | |

**Reference module: Radxa Zero 3W, 8 GB, with eMMC** (RK3566, 4 × A55 @ 1.8 GHz, Wi-Fi 6, 65 × 30 mm, Pi-compatible 40-pin
header). Its header pin functions and mechanical sizes are **provisional** until the module is in hand (the list is in
`AICard/README.md`, as for the LTE card's core board). A faster module with the same header drops straight in; the card
needs no change.

**Not chosen, and why.** A **Raspberry Pi Compute Module 5** is faster (A76 cores, about 3× the LLM speed) and would be
the better choice at the next revision; it needs two 100-pin 0.4 mm connectors whose pinout this design could not check
at the time of writing, and it would need a different (larger) card. A **Jetson Orin Nano** is the real answer for a
7 B model with fast speech, but draws 7–15 W, needs a 12 V-class supply and a fan, and the 80 × 60 mm card cannot cool it.

### What the module can run (estimates: measure on the bench)

| Job | Choice | RAM | Expected on a 4 × A55 module |
| --- | --- | --- | --- |
| Speech to text | whisper.cpp `base.en` (q5), or sherpa-onnx streaming zipformer | 0.2–0.4 GB | a 5 s phrase in about 2–4 s (zipformer: faster than real time) |
| Language model | Qwen2.5-1.5B-Instruct Q4_K_M (≈ 1.1 GB) default; a 3 B model (≈ 2 GB) on 8 GB | 1.1–2.2 GB | about 3–6 tokens/s for 1.5 B: a 30-word answer in 8–10 s. Fine as the offline fallback, slow as the first choice |
| Voice | piper `en_US-lessac-medium` | 0.1 GB | faster than real time |
| Manual search | BM25 over text chunks of the owner's / shop manual | tens of MB | milliseconds |

These are the numbers a designer should plan around, **not measured results**. The agent logs per-stage timings
(`elc-ai --bench`) so the first thing to do with a module in hand is to record them and pick the model size from them.

### Latency targets

| Path | First word spoken within |
| --- | --- |
| Deterministic intent ("what's the oil pressure?", "volume up") | 1.5 s of the end of speech |
| Online answer | 3.5 s |
| Local model | 8 s (the card says "let me think" with a short chime after 2 s so the driver knows it heard) |

## Power

| Rail | From | Load | Notes |
| --- | --- | --- | --- |
| ESP32-S3, CAN, 3.3 V | slot +5 V (pins 1 / 2, 0.5 A PTC) | ≈ 0.15 A at 5 V, radio unused | Always on while the dash is. The card reports on CAN even with no AUX |
| Compute module | **AUX 12 V** (pins 25–27, return 28–30) → TPS54560B buck → 5 V, 5 A | 1–1.5 A typical, 3 A peak | Enabled by the ESP32 only. Same buck as the LTE card (`LTECard/`), with an ESP32-controlled enable |
| Bulk | 470 µF at the module's 5 V, 3 × 47 µF at the buck | | A burst of CPU load must not sag the 5 V |

- **Grounds:** AUX GND is not GND (the LTE card's rule, `LTECard/README.md`): the buck and the module's ground return are
  on AUX GND, the ESP32 side on GND, and only logic crosses, through 1 k resistors.
- **With the key off:** the card is off (the dash 5 V is). Waking the car for a voice command is **not** in version 1. The
  module is shut down cleanly on the SYSTEM POWER notice (below).
- **Back-power:** the ESP32 never drives a line into a module that is off. Its outputs go low (or high-Z) first, and
  every line crossing has 1 k in series.

### Power sequence (the ESP32's job)

| State | Entered when | Does |
| --- | --- | --- |
| OFF | ignition off, disabled in the menu, no AUX, too hot, or the retry limit | Buck disabled |
| BOOTING | key on, enabled, AUX present, not too hot | Buck enabled. Waits up to 90 s for the module's HELLO on the link |
| RUNNING | HELLO received and the module's ALIVE line high | Normal. PING every 2 s, three missed answers = hung |
| STOPPING | ignition off (SYSTEM POWER), disabled, thermal limit, or hung | Sends SHUTDOWN, raises SHUTDOWN_REQ; waits up to 20 s for the module's ALIVE line to fall, then cuts the buck |
| FAULT | three failed boots in a row | Buck disabled, an advisory alert ("AI card: compute module not starting"), retried after 10 min |

The ignition-off notice gives "seconds until the rail may drop" (`can_channels.md`, SYSTEM POWER): the module must be
down in that time. **Read-only root** (overlayfs, writable tmpfs) on the module means a power cut is harmless even
when the clean shutdown is missed. The card's 470 µF and the buck's input capacitance hold the module for a few hundred
milliseconds, enough for the ESP32 to end the session; they are not a ride-through.

## Thermal

A 4 W SoC in a closed dash with a plastic case and no fan:

- Heatsink on the module's SoC (parts side, up to 20 mm tall, so a 6–10 mm sink fits). No fan.
- An NTC on the card (beside the module) is read by the ESP32 (ADC); the module reports its own SoC temperature over the link.
- **Throttle in two steps:** at 70 °C the module is told to cap the model's threads (the agent halves them); at 80 °C
  the ESP32 sends SHUTDOWN and the card goes to OFF; it restarts when under 65 °C. The deterministic intents and the
  wake word run on the ESP32 and keep working.
- A voice assistant is idle most of the time (a few seconds of work per request), so the average is far below the peak.

## Audio

- **Microphone:** none on this card. The audio card's PCM1808 is the microphone, and its data reaches every slot on the
  **rail's MIC line** with BCK / WS (`accessory_ports.md`, I2S rail rules). The ESP32 is an I2S slave on these.
- **Speaker:** the ESP32 asks the audio card for **RET** (NET RAIL, `can_channels.md`) and drives it through a 3-state
  buffer (74LVC125, enable pulled to *off*, 33 Ω in series) only while it holds the grant. The audio card mixes it into
  the speakers and ducks the music.
- **Rates:** the rail runs at the audio card's 44.1 or 48 kHz, 32-bit slots, stereo. Speech models use 16 kHz mono (the
  recogniser) and 22.05 kHz mono (the voice). The ESP32 resamples both ways (`ai_audio`, host-tested), so the module
  only ever sees 16 kHz mono in and its own voice rate out.
- **Echo:** the music is ducked while the assistant talks, and the microphone is not used for wake-word detection while
  the card itself is speaking (v1). Barge-in (interrupting the voice with the wake word) is for later and needs the
  ESP32's acoustic echo cancellation fed with the card's own output as the reference.
- **Fail-safe:** if the audio card (the rail master) is not there, the card cannot hear or speak. It says so in the menu
  and shows answers as text only.

## The two networks

The card can reach the internet two ways, and uses what exists:

| Uplink | How | Used for |
| --- | --- | --- |
| **Wi-Fi client** on the compute module | Joins the phone's hotspot or a home network; credentials kept on the module (set from the menu, or a file on the module's storage) | The online model and web search. This is the main path |
| **The LTE gateway card's results** on CAN (ONLINE channel) | Free: PLACE, WEATHER, TIMEZONE, MESSAGE, STATUS are already broadcast | "Where am I?", "will it rain?", the time zone: **works offline of the assistant's own network**, and its ONLINE STATUS bit0 (internet up) is a hint that a lookup is worthwhile |

The cluster itself still never uses Wi-Fi; this is the assistant card's own radio, off unless the driver enables
*Online lookups*. A gateway that relays arbitrary lookups over CAN (a QUERY / ANSWER pair on ONLINE, a few hundred bytes
each way, about 1 % of the bus) would let the assistant use the LTE card's data without a phone; **it is not part of
version 1** and would be a change to `can_channels.md`. Everything above works without it.

**Connectivity detection** (the agent, `connectivity.py`): a lookup is *possible* when the module has a default route
and the provider answers a small HEAD / models request within 1.5 s, re-checked every 15 s (and at once when a request
fails), with hysteresis (two bad checks to go down, one good check to come up). The card publishes the state to the menu
(*Online: ready / offline*) and the answer's source ("local" / "online") is shown on the toast.

## Routing: online or on-board

```
speech → text (always local) ─► intent engine ─ matched ─────────────► answer from live data / command (no model)
                              └ not matched ─► needs fresh facts? ─ yes, and online ─► online model (web search)
                                              │                                          └ error / slow / refusal ─► local model
                                              └ no / offline ─► local model (grounded with sensors + manual)
                                                                  └ answers UNKNOWN, and online ─► online model
```

"Needs fresh facts" is a small, tested rule set (news, today, latest, near me, open now, price, score, who is / what is
the current…) plus the menu setting: *Auto*, *Always online* or *Offline only*. Sensor and car-state questions are
**never** sent online. Vehicle context sent with an online question is limited to what the question needs (units, the
town name, never the position more precise than that, never the VIN or an identifier).

## Slot use

| Pins | Signal | Used |
| --- | --- | --- |
| 1 / 2 | +5 V | ESP32-S3, CAN transceiver, 3.3 V regulator |
| 3 / 4, 7, 19 / 20 | GND | logic ground |
| 5 / 6 | CANH / CANL | MCP2562, never terminated |
| 11 | slot ID | 10 k to 3.3 V, ADC |
| 15 / 16 | I2S BCK, WS | inputs (slave) |
| 17 | I2S MIC | input: the microphone / aux data |
| 18 | I2S RET | 3-state output through the 74LVC125 and 33 Ω |
| 25–27 / 28–30 | AUX 12 V / AUX GND | the module's buck |
| 8, 9, 10, 12–14, 21–24 | switched 12 V, 3.3 V, constant 12 V, WAKE, LAMP, REV, audio bus, MCLK | not connected |

Goes in **any slot** (nothing tall: the module and heatsink stand about 17 mm). Node `0x14 + (slot − 1)`, product `0x0007`.

## What the ESP32 lets the AI do (the allow-list)

The assistant asks the ESP32 to send CAN transfers (`ai_link.md`, CAN_TX); the ESP32 sends only:

| Channel | Opcodes | Limit |
| --- | --- | --- |
| AUDIO_CMD | VOLUME (set / step / mute), MEDIA, SOURCE, PLAY (UI sounds, priority ≤ 1) | 5 per second |
| ALERT | the card's own DEFINE / STATE, severity ≤ advisory | 2 per second |
| UI | the card's own VALUE / TEXT | 5 per second |
| INPUT | KEY only: up / down / select / back / home (to drive the menu by voice) | 5 per second |
| LOG | LINE | 10 per second |

Everything else (SYSTEM, SENSOR, calibration, higher severities, other nodes' private ranges) is **refused and logged**.
A request addressed to a node that has not announced is dropped. The ESP32 also refuses to send anything until the
module has said HELLO, and a module that floods the link is reset.

## Environment and robustness

- **Vibration:** the module is held by **two M2.5 standoffs** into the card as well as its 2×20 socket (the socket alone
  walks out). The card has two mounting holes for them (the only holes: the slot and the case hold the card itself).
- **Brown-out:** the buck's enable divider (442k / 90.9k, as on the main board and the LTE card) starts it above about 6.5 V
  and stops it below about 5 V; a crank that sags AUX under that switches the module off and the ESP32 restarts it
  (BOOTING, then RUNNING) when AUX is back. The read-only root makes that harmless. Ride-through is not a goal.
- **Transients:** the 12 V entry is protected on the main board (reverse FET, PTC); the buck takes 60 V.
- **Updates:** the ESP32 flashes over its USB-C (or the UART header); the module's image is written to its eMMC from a
  PC over the module's own USB-C (vendor tool). The agent is updated in place over SSH on the module's Wi-Fi
  (or a USB stick), and **the ESP32 firmware can later be updated over BULK** (`can_channels.md`).
- **Blank chips:** the card from assembly has a blank ESP32 (programming header, USB-C, BOOT / RESET as every card) and a
  module that is a separate, pre-flashed part.

## Decisions to review

1. **Compute module.** A Pi-header module (Radxa Zero 3W) now; a CM5-based card when its connector pinout can be checked.
2. **Uplink.** The module's own Wi-Fi to a phone hotspot. A CAN relay on the LTE card is the alternative (no phone needed).
3. **Online provider.** Claude (Messages API, web search tool). The setting is a URL and a key on the module, so another
   provider can replace it; the key never goes on CAN.
4. **Wake word.** An Espressif WakeNet word (the built-in ones) on the ESP32-S3; a custom word needs Espressif's
   training service. Press-to-talk (`F1`) works with no wake word at all.
5. **Key-off use.** Not in v1 (the card is off with the dash).
