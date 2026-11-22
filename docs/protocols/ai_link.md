# AI link: ESP32-S3 ↔ compute module

**Status: specification v1, implemented by `firmware/ai_card/src/ai_link.*` (ESP32 side) and
`firmware/ai_agent/elc_ai/link.py` (module side), with matching host tests.** Hardware: `docs/hardware/ai_card.md`.

One UART, **1,500,000 baud, 8N1, no flow control** (the protocol has its own credit), 3.3 V, between the AI card's
ESP32-S3 (UART1) and the compute module's header UART (pins 8 / 10). Both sides send frames; there is no master.

## Framing

```
frame on the wire:  COBS( type u8 | seq u8 | payload ... | crc16 lo | crc16 hi )  0x00
```

- **COBS** (consistent overhead byte stuffing) removes every `0x00` from the body, so `0x00` ends a frame and a receiver
  resynchronises at the next one. A leading `0x00` is sent at start-up and after any error.
- **CRC-16/CCITT-FALSE** (poly `0x1021`, init `0xFFFF`, no reflection, no final XOR; `"123456789"` → `0x29B1`) over
  `type | seq | payload`, the same CRC as the CAN channels, low byte first.
- `seq` counts frames the sender has sent (wraps at 256). A gap is counted for diagnostics; nothing is retransmitted
  (audio is real time, control messages are repeated by state).
- Largest frame body: 1 + 1 + 1020 + 2. Audio frames stay at 640 bytes (20 ms at 16 kHz).
- A bad CRC, a bad COBS run or an over-long frame drops that frame only.

## Messages

Little-endian. Types `0x00`–`0x7F` go ESP32 → module, `0x80`–`0xFF` module → ESP32.

### ESP32 → module

| Type | Name | Payload | When |
| --- | --- | --- | --- |
| `0x01` | HELLO | protocol u8 (1), card fw u16, slot u8, node u8, flags u8 (bit0 mic muted, bit1 AUX present) | at start, and when asked |
| `0x02` | PING | counter u8 | every 2 s while running |
| `0x03` | SHUTDOWN | reason u8 (1 ignition off, 2 disabled, 3 hot, 4 restart, 5 reset) | before power-off; SHUTDOWN_REQ is raised as well |
| `0x04` | SETTINGS | online u8 (0 auto, 1 always, 2 offline only), speak u8, voice level u8 (%), flags u8 (bit0 forget the conversation) | after READY, and when a menu setting changes |
| `0x10` | LISTEN | reason u8 (1 wake word, 2 key, 3 menu), keyword id u8 | a session starts |
| `0x11` | AUDIO | PCM s16le mono 16 kHz, 80–640 bytes | during a session, 20 ms per frame |
| `0x12` | CANCEL | reason u8 (1 key, 2 timeout, 3 mic muted, 4 rail lost, 5 the audio card gave no RET) | the session ended on the ESP32 |
| `0x13` | SPOKEN | state u8 (0 done, 1 started, 2 aborted: RET denied or withdrawn) | how the speech went |
| `0x20` | CAN_RX | channel base u16, source node u8, transfer payload | forwarded channel messages the assistant subscribed to |
| `0x21` | CAN_NODES | count u8, then (node u8, type u8, product u16) × count | the ANNOUNCEd nodes, 1 per 10 s and on change |
| `0x22` | CTX | key u8, value s32 (see below) | the card's own readings: rail up, mic level, temperatures, slot |
| `0x30` | CAN_TX_RESULT | tag u8, result u8 (0 sent, 1 refused, 2 rate limited, 3 failed) | answer to CAN_TX |

### Module → ESP32

| Type | Name | Payload | When |
| --- | --- | --- | --- |
| `0x81` | READY | agent version u16, flags u8 (bit0 online provider configured, bit1 local model loaded), name text | the agent is up (HELLO's answer) |
| `0x82` | PONG | counter u8, soc temperature s8 (°C), load u8 (%), flags u8 (bit0 online up, bit1 throttled) | answer to PING |
| `0x83` | SHUTDOWN_ACK | none | about to halt |
| `0x90` | STATE | state u8 (0 idle, 1 listening, 2 thinking, 3 speaking), source u8 (0 none, 1 intent, 2 local model, 3 online) | the assistant's state, for the LED and the menu |
| `0x91` | LISTEN_STOP | none | the recogniser has the end of the phrase: stop sending audio |
| `0x92` | TEXT | kind u8 (1 heard, 2 answer, 3 error), UTF-8 text ≤ 200 bytes | what it heard / said, for the menu and toasts |
| `0xA0` | SPEECH | rate code u8 (0 16 kHz, 1 22.05 kHz, 2 24 kHz, 3 44.1 kHz, 4 48 kHz), PCM s16le mono, ≤ 960 bytes | the voice. Sent paced (credit below) |
| `0xA1` | SPEECH_END | none | no more audio for this answer |
| `0xA2` | SPEECH_CANCEL | none | stop and drop what is queued |
| `0xB0` | CAN_TX | tag u8, channel base u16, transfer payload | ask the ESP32 to send a transfer as the card's node |
| `0xB1` | SUBSCRIBE | channel count u8, channel base u16 × count | which channels to forward in CAN_RX (default: SYSTEM, SENSOR, ALERT, ONLINE, AUDIO_CMD) |
| `0xB2` | CTX_REQUEST | none | send CAN_NODES and CTX now |
| `0xF0` | LOG | level u8, text | into the ESP32's console, and LOG on CAN at ≤ 10 / s |

### Speech credit

The ESP32 holds the voice in a ring buffer (about 1.5 s at the rail rate) and does not want more than that queued.
It tells the module how much room it has: `0x31` **SPEECH_CREDIT** (ESP32 → module): bytes the module may send now u16,
sent every 100 ms while speaking and after each drain. The module sends SPEECH frames only within the last credit.
Without credit it waits (a module that does not see credit for 2 s stops the answer).

### CTX keys (the card's own state, `0x22`)

| Key | Meaning |
| --- | --- |
| 1 | rail up (1 = BCK / WS present) |
| 2 | RET granted (0 / 1) |
| 3 | microphone level, dBFS × 10 (last 100 ms) |
| 4 | card temperature, °C × 10 |
| 5 | AUX 12 V present (0 / 1) |
| 6 | the audio card's sample rate / 10 |
| 7 | mic muted (0 / 1) |

## Control lines

| Signal | Direction | Header pin | Meaning |
| --- | --- | --- | --- |
| ALIVE | module → ESP32 | 29 (GPIO5) | high while the agent runs; the module drives it high from its service, low in `ExecStop` |
| SHUTDOWN_REQ | ESP32 → module | 31 (GPIO6) | high = please halt. A `gpio-shutdown`-style overlay or the agent's watcher reads it; it works when the link does not |
| ATTN | both | 33 (GPIO13) | spare (input on both sides) |

## A session, end to end

```
ESP32: wake word            ──► LISTEN(wake)           module: STATE(listening)
ESP32: mic audio            ──► AUDIO × n                      (recogniser runs on the stream)
module: end of phrase       ──► LISTEN_STOP            ESP32: stops forwarding audio
module: TEXT(heard)         ──► menu UI text
module: STATE(thinking)     ──► LED
module: CAN_TX(AUDIO_CMD volume …)   or   the answer text …
module: STATE(speaking), SPEECH × n (within credit), SPEECH_END
ESP32: RAIL request → granted → RET on → plays → RET off → release   ──► SPOKEN(done)
module: STATE(idle)
```

- **Timeouts (ESP32):** a session with no LISTEN_STOP after 12 s of audio is cancelled; a missing STATE change for 20 s
  after LISTEN_STOP is cancelled; speech with no credit use for 2 s is aborted.
- **Mic muted** (menu): no wake word, no LISTEN, no AUDIO; the module is told in HELLO and CTX.
- **Link lost:** three PINGs without PONG = the module is hung (the power sequencer restarts it, `ai_card.md`).
