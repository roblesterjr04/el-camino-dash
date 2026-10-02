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
computer's clock (`0x220`), and the **AM/FM radio card** (`tools/radio_sim.py`, in slot 2 = node `0x15` unless
`--radio-slot` says otherwise; `--no-radio` turns it off). The radio speaks the channel protocol as the card's
firmware will ([`radio_card.md`](../hardware/radio_card.md), CAN): heartbeat, NET ANNOUNCE / QUERY / RAIL, its menu
page on UI, signal and SNR on SENSOR, LOG lines. Its stations are made up. `:radio status`, `:radio tune 98.3`,
`:radio seek up`, `:radio source on` (pretend the audio card selected it: the keys then seek and mute, and it asks the
audio card for RET), `:radio on|off`. Unlike the drive and the clock it also runs with `--gateway`, since no radio card
exists yet. `python3 tools/radio_sim.py --test` checks its framing and logic with no hardware.

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
