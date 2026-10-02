# CAN over USB (the USB link)

Every board that runs `elc_can` (the three round displays, the console, the audio bridge) carries its CAN traffic
over its USB serial port as well, when a relay on a computer asks for it. On the bench the computer *is* the bus: plug
the boards into one Mac, run the relay, and they talk as they will in the truck, with no transceivers or CAN wiring.
In the truck, a laptop on any one board can watch and send on the real bus (gateway mode).

It is part of the shipped firmware: there is no build flag and no bench build. Without a relay the port is the
board's text console, as always.

```bash
python3 tools/can_relay.py                    # every USB serial port, peer mode
python3 tools/can_relay.py --frames           # with a frame log (:filter 0x215 to narrow it)
python3 tools/can_relay.py --gateway center   # on the truck: the center bridges the laptop onto the bus
```

Type `center help`, `console state`, `all can` … to reach a board's shell (`:ports` lists the boards). Code:
`firmware/elc_can/include/elc_usb_link.h`, `src/elc_usb_link.cpp`; host tests in `elc_can/test/host`.

The relay also stands in for boards that are not on the bench: a simulated drive (sensor frames `0x100`–`0x104`), the
computer's clock (`0x220`), and the slot cards that are specified but not built. Each card speaks the
[channel protocol](can_channels.md) as its firmware will: heartbeat, NET ANNOUNCE / QUERY / IDENTIFY / RESTART, and
its own channels. The values are made up and settings live in memory only.

| Card | Slot, node | What it does |
| --- | --- | --- |
| `radio`: AM/FM radio card (`tools/radio_sim.py`, [`radio_card.md`](../hardware/radio_card.md)) | 2, `0x15` (`--radio-slot`) | its menu page on UI, signal and SNR on SENSOR, LOG lines; asks the audio card for RET while it is the source; seeks and mutes on MEDIA from the audio card |
| `audio`: audio card (`tools/cards_sim.py`, as are the rest) | 1, `0x14` | AUDIO_CMD STATUS, PLAY / STOP / VOLUME / MEDIA with DONE and ducking, the media keys and encoder 1, RET arbitration on NET RAIL (the voice card outranks the others, the radio ranks lowest; RET is freed when the holder's heartbeat stops for 3 s); a page with Source (Bluetooth / Radio), Volume, Fader, Balance. With Source = Radio it forwards play / next / previous to the radio as MEDIA |
| `gps`: GPS card | 3, `0x16` | speed, heading, altitude and satellites on SENSOR, following the simulated drive (no fix for the first 6 s); a page with the fix and satellites |
| `lte`: LTE gateway | 4, `0x17` | NET GATEWAY STATUS; ONLINE PLACE, WEATHER, TIMEZONE, ONLINE STATUS, a MESSAGE a minute after start; answers REFRESH, MESSAGE READ and GATEWAY CMD; a page with the network, signal, data used and data cap |
| `voice`: AI voice card | 5, `0x18` | asks the audio card for RET, speaks for 3 s once granted, releases it |
| `trans`: transmission-temperature card ([`can_channels.md`](can_channels.md), Examples) | 6, `0x19` (`--trans-slot`) | trans temp on SENSOR at 5 Hz, its own ALERTs (caution at 110 °C, warning at 125 °C) with STATE at 1 Hz, an Offset item |

Commands: `:sim list`, `:sim on|off CARD`, and per card `:radio status | tune 98.3 | seek up | source on | on | off`,
`:audio status | source bt|radio | volume 40 | mute | play 3 [priority] | stop | media next`, `:gps status | fix off`,
`:lte status | message [text] | refresh | offline | online`, `:voice say [text]`,
`:trans status | temp 120 | mode own|center` (`center`: the example's center-raised alerts, SENSOR flag bit2, instead
of its own). `--no-sim audio,gps` leaves cards off; `--no-radio` is `--no-sim radio`. The cards hear each other as
well as the boards. The audio card's Source has no CAN message, so the relay tells the radio when it changes
(`:radio source on` sets it on the audio card). Unlike the drive and the clock the cards also run with `--gateway`,
since none of them exists yet. `python3 tools/chan_sim.py --test`, `radio_sim.py --test` and `cards_sim.py --test`
check the framing and each card's logic with no hardware; `can_relay.py --test` runs the cards together (no pyserial
needed).

## Wire format

Records sit between the console text. A record is `0x00`, the COBS encoding of (payload + CRC-16/CCITT-FALSE of the
payload, little-endian), `0x00`. Console text never contains `0x00`. A record that fails its check is dropped and its
closing `0x00` is taken as the start of the next one, so the stream recovers by itself after noise, a reset or a baud
change. Multi-byte fields are little-endian.

| Type | Direction | Payload |
| --- | --- | --- |
| `'F'` frame | both | `'F'`, CAN id u16 (11-bit), length u8 (0–8), data |
| `'K'` keepalive | relay → board | `'K'`, version u8 (1), mode u8 (0 peer, 1 gateway), baud u32 (0 = stay) |
| `'S'` status | board → relay, answering each keepalive | `'S'`, version u8, node u8 (0xFF = not set), node type u8, mode u8, baud u32 (the rate after this record; 0 = native USB), CAN controller running u8, fw version u8 |

## Behaviour

- **On:** the first keepalive. **Off:** no keepalive for 3 s. The relay sends one a second. Off, the board drops
  queued frames, goes back to peer mode and to 115200.
- **Baud:** boards with a USB-UART chip (the P4 displays' CH343, the DevKitC's CP210x) move to the keepalive's rate
  after sending their status at the old one, and the relay follows. Allowed: 115200, 230400, 460800, 921600, 1500000,
  2000000. The relay asks 2000000 of a CH343 and 921600 of a CP210x. Native USB (the console's ESP32-S3) has no rate.
  A relay that restarts within the 3 s finds the board on the fast rate: it hears nothing, falls back to 115200, and
  the board has dropped back by then.
- **Peer mode:** the board sends its own frames up (heartbeats included), and frames from the relay reach only this
  board, through the same handler as frames from the bus. The relay copies each board's frames to every other board.
- **Gateway mode:** also every frame the board hears on the bus goes up, and frames from the relay go out on the bus.
  One gateway per bus; don't mix it with peer boards that share that bus, or they hear frames twice.
- `ElcCan::running()` is true while the CAN controller or the link works, so the firmware sends either way;
  `ElcCan::txFree()` reports a full queue's room on the link alone.
- The shells read through `elc_usb_available()` / `elc_usb_read()`, which hand over the text only.
- The link is not a bus: no arbitration, no error frames, no bus-off. Those only happen on real CAN.
- `shot` (screenshots) writes raw pixels on the same port: quit the relay before `tools/screenshot.py`.
- `can` on each board's console ends with a `usb:` line (relay, mode, baud, frames up / down, drops, bad records).
