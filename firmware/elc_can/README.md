# elc_can: the shared CAN library

The CAN code every node on the DigiDash bus shares: the IDs and node numbers, the codecs for the cluster's own frames
(`0x100`–`0x226`, `0x400`+ accessory ranges, `0x700` heartbeats), the TWAI driver wrapper with the heartbeat and
bus-off recovery, the channel protocol (transport, NET, and the SYSTEM / LOG / SENSOR / ALERT / INPUT / AUDIO_CMD / UI
messages), the alert manager, the accessory menu pages and the USB CAN link. It is a PlatformIO library
(`library.json`, name `elc_can`); the parts without Arduino code also build on a Mac for the host tests.

The specs are [`docs/protocols/can_bus.md`](../../docs/protocols/can_bus.md) (the bus, the cluster's frames, the
node numbers), [`docs/protocols/can_channels.md`](../../docs/protocols/can_channels.md) (the channel protocol) and
[`docs/protocols/usb_link.md`](../../docs/protocols/usb_link.md) (CAN over USB). This README says where each part
lives and how to use it; it does not repeat the wire formats. Overview of all the firmware:
[`../README.md`](../README.md).

```bash
test/host/run_tests.sh      # codec, channel and alert-manager tests on the Mac (no ESP32 toolchain needed)
```

## Who uses it

| User | How | What it takes |
| --- | --- | --- |
| [`esp32_p4_display`](../esp32_p4_display/) (the three round screens, nodes `0x01`–`0x03`) | `lib_deps = file://../elc_can` | everything; `accessory_can` for the menu's Accessories page, `elc_alerts` on the center |
| [`esp32_p4_ui`](../esp32_p4_ui/) (shared screen code) | include path | codecs, `ElcCan`, channels, `accessory_can` |
| [`esp32s3_console`](../esp32s3_console/) (node `0x13`) | `lib_deps = file://../elc_can` | `ElcCan`, channels, `accessory_can`, codecs |
| [`esp32_classic_bt_audio`](../esp32_classic_bt_audio/) (audio bridge / audio card) | `lib_deps = file://../elc_can` | `ElcCan`, channels (it announces as the audio sink), the audio and phone codecs |
| [`sensor_card`](../sensor_card/) (ATmega2560) and [`sensor_card_stm32`](../sensor_card_stm32/) | `build_src_filter` of four sources | `elc_chan.cpp`, `elc_chan_msg.cpp`, `elc_can_codec.cpp`, `elc_usb_link.cpp` (no TWAI: each card has its own CAN driver and USB port, `ELC_USB_PORT_EXTERNAL`) |
| `tools/gauge_preview` (host renderer) | compiles the sources | `elc_can_codec`, `elc_chan`, `elc_chan_msg`, `accessory_can` |

## Status

- Host tests pass for the codecs, the channel transport and NET, the channel messages, the accessory pages and the
  alert manager (`test/host/run_tests.sh`).
- The TWAI driver and the USB link run on the round screens, the console and the audio bridge on the bench (the root
  [`README.md`](../../README.md#status): bench testing only, nothing is in the car).
- Not implemented in this library or its users yet (from [`can_channels.md`](../../docs/protocols/can_channels.md)):
  INPUT senders and receivers, BULK and AUDIO_STREAM users, the accessory settings backup on the center's SD card.
  The codecs for INPUT exist (`elc_input_*`).

## Layout

| File | Role |
| --- | --- |
| `include/elc_can_ids.h` | IDs of the cluster's frames, screen numbers, node numbers, the accessory ranges (`elc_can_slot_id()`, `elc_can_slot_of()`), the audio bridge's message offsets, the rev 2 slot from the slot ID pin's voltage (`elc_can_card_slot_from_mv()`), `ElcCanNodeType`. Header only |
| `include/elc_can_codec.h`, `src/elc_can_codec.cpp` | `ElcCanFrame` and an encode / decode pair for every cluster frame: sensors `0x100`–`0x104` (`ElcCanSensorDecoder` collects the five), cluster state `0x210`, settings `0x212`, center status `0x213`, menu `0x216`–`0x219`, time `0x220`, heartbeat `0x700 + node`, the audio bridge's transport, text, album art and phone frames, phone commands `0x215`. Chunked texts and album art have receivers (`ElcCanTextDecoder`, `ElcCanAudioTextDecoder`, `ElcCanPhoneTextDecoder`, `ElcCanArtReceiver`) |
| `include/elc_can.h`, `src/elc_can.cpp` | `ElcCan`: TWAI driver install (500 or 250 kbit/s, accept all), single-shot send (BULK data: with the controller's retries), receive into a handler from `poll()`, 1 Hz heartbeat, bus-off recovery, the nodes heard, the USB link alongside. `elc_can_read_slot_id()` (rev 1 straps) and `elc_can_read_card_slot()` (rev 2 slot ID pin) |
| `include/elc_chan.h`, `src/elc_chan.cpp` | The channel protocol's transport: the tail byte, multi-frame transfers with CRC-16/CCITT-FALSE (`elc_ch_frames()`), reassembly per CAN ID (`ElcChRx`), `ElcChWriter` / `ElcChReader`, NET ANNOUNCE / QUERY / IDENTIFY / RESTART / RAIL, and `ElcChannels` (one per board: announces, answers queries, keeps the announced nodes, asks a node that heartbeats without having announced). `ElcChService`: something more a board runs on its channels (sees every frame and transfer, polled with them; `addService()`) |
| `include/elc_bulk.h`, `src/elc_bulk.cpp` | BULK (`can_channels.md`): the message codecs, CRC-32, and both ends of a WRITE: `ElcBulkWriter` (to one node or, multicast, to every node that takes it; paced; serves the slowest receiver first) and `ElcBulkReceiver` (credit-based WANTs, holds chunks past a lost one and asks for that one alone, the CRC, a sink that can take its time to finish) |
| `include/elc_fw.h`, `src/elc_fw.cpp` | The firmware profile: the `.elcfw` header, `elc_fw_offer()`, and `ElcFwTarget` (a service: takes WRITEs of its own product and build, checks the header before flash is touched, writes through an `ElcFwFlash`, restarts when done). Host-tested |
| `include/elc_fw_esp32.h`, `src/elc_fw_esp32.cpp` | The target on an ESP32: the spare OTA slot through the Update library (staged in PSRAM when there is room, else written as it arrives; written from its own task), keeping a new image after `ELC_FW_GOOD_MS` of healthy running or rolling it back (`verifyRollbackLater`), the `fw` console command. Empty on other targets |
| `include/elc_chan_msg.h`, `src/elc_chan_msg.cpp` | Payload codecs for SYSTEM, LOG, SENSOR, ALERT, INPUT, AUDIO_CMD and UI (`elc_sys_*`, `elc_log_*`, `elc_sensor_*`, `elc_alert_*`, `elc_input_*`, `elc_audio_*`, `elc_ui_*`), the quantity numbers (`ELC_Q_*`), the sources (`ELC_SRC_*`) and the audio sink's fixed item numbers (`ELC_SINK_ITEM_*`) |
| `include/elc_alerts.h`, `src/elc_alerts.cpp` | `ElcAlertManager`: the dash's one alert list (every node's DEFINE + STATE, its own alerts as node `0x01`), chimes on the audio sink (PLAY / STOP), acknowledgement (ACK), "source lost" after 3 s of silence, SUMMARY. Runs on the center |
| `include/accessory_can.h`, `src/accessory_can.cpp` | Every accessory's UI page (items, values, texts) kept on each screen and on the console; SET and VIEW back to the node. The Accessories page of each menu reads it. `accessory_can_tank_gal()` gives the sensor card's tank size |
| `include/elc_usb_link.h`, `src/elc_usb_link.cpp` | CAN over the USB serial port: COBS records with CRC between the console text, peer and gateway modes, keepalive timeout 3 s. `elc_usb_begin()` replaces `Serial.begin()`, `elc_usb_available()` / `elc_usb_read()` replace `Serial.available()` / `Serial.read()` |
| `include/elc_sensor_units.h` | Sensor ADC counts to the units the gauges show (fuel %, oil psi, coolant °F from a 100k NTC, charge -100..100) and the sensor flag bits. Header only, shared by `esp32_p4_common` and the console |
| `test/host/` | `run_tests.sh` builds four test programs with `-fsanitize=address,undefined`: `test_elc_can_codec.cpp`, `test_elc_chan.cpp`, `test_elc_alerts.cpp`, `test_elc_bulk.cpp` (a simulated bus: multicast, loss, refusals, a streaming source, a slow flash, abort). `fw_target_pipe.cpp` is the firmware target on a pipe, for `tools/fw_update.py --test` |

## Using it on a board

The loop contract, as the screens, the console and the audio bridge use it:

```cpp
#include <elc_can.h>

static ElcCan s_can;
static ElcChannels s_chan;

void setup() {
  elc_usb_begin();                                   // Serial at 115200 with the USB CAN link
  s_can.onFrame(on_frame, nullptr);                  // every frame, from poll() (loop context)
  s_can.begin(ELC_CAN_TX_PIN, ELC_CAN_RX_PIN, node, ElcCanNodeType::Display);
  ElcNetAnnounce me;                                 // name, type, product, produces / consumes maps
  s_can.attachChannels(&s_chan, me);                 // heartbeat flag bit0 = speaks the channel protocol
  s_chan.onTransfer(on_transfer, nullptr);           // complete channel transfers
}

void loop() {
  s_can.poll();                                      // USB link, bus-off recovery, receive, heartbeat, channels
}
```

- `begin()` returns false and leaves the driver off when a pin is `-1` or the driver fails. The USB link still works,
  so `running()` is true while a relay talks to the board; `stats().running` is the controller alone.
- `send()` puts a frame on the bus and, while a relay is active, up the USB port. Frames are single-shot.
- `txFree()` is the room left in the 16-frame transmit queue. Bulk senders (album art) fill only that much per pass.
- `nodeAlive(node, max_age_ms)` (default 3 s) and `nodeInfo(node)` come from heartbeats. `describe()` is the
  one-line summary the `can` console commands print.
- Off the ESP32 (host builds, the AVR) the TWAI part compiles to an inert driver (`ELC_CAN_HAVE_TWAI` is 0).

A board short of RAM shrinks the channel buffers with build flags; the ATmega2560 sensor card does.

## Configuration (build flags)

| Flag | Default | Meaning |
| --- | --- | --- |
| `ELC_CAN_TX_PIN` / `ELC_CAN_RX_PIN` | none here (each board sets them) | TWAI pins; `-1` turns CAN off. Screens 34 / 36, audio bridge 32 / 33, console 5 / 6 |
| `ELC_CAN_BITRATE_KBPS` | 500 | 500 or 250 (every node must match; `can_bus.md` says when to use 250) |
| `ELC_CAN_FW_VERSION` | 1 | the firmware version sent in the heartbeat and ANNOUNCE |
| `ELC_CH_PAYLOAD_LIMIT` | 256 | longest channel transfer sent or accepted (the ATmega2560 sensor card uses 160) |
| `ELC_CH_RX_BUFFERS` | 6 | reassembly buffers (sensor card rev A: 3) |
| `ELC_CH_RX_DUPS` | 48 | remembered transfer IDs for the duplicate rule (sensor card rev A: 16) |
| `ELC_USB_PORT_EXTERNAL` | not set | the board brings its own USB-link port (the sensor cards' `card_usb.cpp`) |
| `ELC_FW_VARIANT` | set by `scripts/elc_fw.py` | this build's PlatformIO environment: a firmware WRITE for another build is refused (`"unknown"` refuses every one that names a build) |
| `ELC_FW_GOOD_MS` | 20000 | a new image is kept after running healthy this long |
| `ELC_FW_ROLLBACK` | 1 | 0: accept every new image as soon as it boots (no rollback) |

## How it fits the system

- **Physical layer and node numbers:** [`can_bus.md`](../../docs/protocols/can_bus.md#physical-layer). Screens
  `0x01`–`0x03` from the position strap, rev 1 carriers `0x10`–`0x12`, the console `0x13`, rev 2 slot cards
  `0x14 + slot - 1`, `0x1A`–`0x1B` other cable-port devices.
- **The cluster's frames** (`0x100`–`0x226`, the accessory ranges `0x400 + 0x40 · (node - 0x10)`, heartbeats
  `0x700 + node`): [`can_bus.md`](../../docs/protocols/can_bus.md#messages). These are internal and may change with
  the display firmware.
- **The channels** (`base + node`: ALERT `0x020`, INPUT `0x040`, SYSTEM `0x060`, SENSOR `0x120`, AUDIO_CMD `0x240`,
  UI `0x260`, AUDIO_STREAM `0x300`, BULK `0x320` / `0x340`, ONLINE `0x360`, NET `0x3C0`, LOG `0x3E0`):
  [`can_channels.md`](../../docs/protocols/can_channels.md). This is the public interface for accessories.
- **USB link:** [`usb_link.md`](../../docs/protocols/usb_link.md); the relay is `tools/can_relay.py`.

## Extending it

**A new cluster frame** (internal traffic):

1. Pick a free ID in `can_bus.md`'s message table and add it there first (specs first, see the root
   [`README.md`](../../README.md#contributing)).
2. Add the `ELC_CAN_ID_*` constant to `elc_can_ids.h`.
3. Add a struct and `elc_can_encode_*` / `elc_can_decode_*` to `elc_can_codec.h` / `.cpp`. Payloads are little-endian.
   A decoder should accept a longer frame from a newer sender and ignore the extra bytes.
4. Add a round-trip test to `test/host/test_elc_can_codec.cpp` and run `test/host/run_tests.sh`.
5. Handle it in the boards' `onFrame` handlers. Receivers ignore IDs they do not know, so the other boards need no
   reflash.

**A new accessory frame:** put it in the node's own range with `elc_can_slot_id(node - 0x10, msg)`, or (better, for
anything another board depends on) use a channel.

**A new channel message:** define it in `can_channels.md`, add the opcode and the codec to `elc_chan_msg.h` / `.cpp`,
test it in `test/host/test_elc_chan.cpp`, and send it with `ElcChannels::send(base, writer)`. Trailing bytes a later
version adds must be ignored by the decoders.

## Known issues and open items

- `can_bus.md` says the audio bridge sends heartbeat flag bit0 = 0. The bridge attaches the channels
  (`arduino/sketches/esp32_audio_bridge/esp32_audio_bridge.ino`), so `ElcCan::heartbeat()` sends bit0 = 1.
- Some comments are left over from the I2C audio link and the planned console (`elc_can_codec.h`: "same limit as the
  I2C snapshot"; `elc_can_ids.h`: the console "(planned)").
- See [`can_channels.md`](../../docs/protocols/can_channels.md), *Open questions*, for the protocol's own open items.
