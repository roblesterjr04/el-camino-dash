#pragma once

// CAN over the USB serial port (docs/protocols/usb_link.md). Every board that runs elc_can carries it, with no build
// flag: it stays off until a relay on a computer (tools/can_relay.py) sends keepalives, so in the car, or with a
// plain serial monitor, the port is the text console it always was.
//
// On the wire, records sit between the console text: 0x00, COBS(payload + CRC-16/CCITT-FALSE, little-endian), 0x00.
// Text never contains 0x00, so the two are told apart byte by byte.
//
// Modes, chosen by the relay:
// - peer: the board sends its own frames up, and frames from the relay reach only this board (the bench, where the
//   relay is the bus).
// - gateway: the board also sends up every frame it hears on the CAN bus, and frames from the relay go out on the
//   bus (a laptop on one board of the truck). One board per bus.

#include <stddef.h>
#include <stdint.h>

#include "elc_can_codec.h"

static const uint8_t ELC_USB_VERSION = 1;

// record types (first payload byte)
static const uint8_t ELC_USB_REC_FRAME = 'F';      // id u16, len u8, data[len]                  (both ways)
static const uint8_t ELC_USB_REC_KEEPALIVE = 'K';  // version u8, mode u8, baud u32 (0 = keep)   (relay -> board)
static const uint8_t ELC_USB_REC_STATUS = 'S';     // version u8, node u8, type u8, mode u8, baud u32, can u8, fw u8
                                                   //                                           (board -> relay)

enum class ElcUsbMode : uint8_t { Peer = 0, Gateway = 1 };

static const uint32_t ELC_USB_DEFAULT_BAUD = 115200;
static const uint32_t ELC_USB_TIMEOUT_MS = 3000;  // no keepalive for this long: the link is off
static const size_t ELC_USB_MAX_PAYLOAD = 16;     // the biggest record before its CRC (a frame: 1 + 2 + 1 + 8)
static const size_t ELC_USB_MAX_WIRE = ELC_USB_MAX_PAYLOAD + 2 + 2 + 2;  // + CRC, COBS overhead, two delimiters

struct ElcUsbKeepalive {
  uint8_t version = ELC_USB_VERSION;
  ElcUsbMode mode = ElcUsbMode::Peer;
  uint32_t baud = 0;  // switch to this rate after the reply (0 = stay)
};

struct ElcUsbStatus {
  uint8_t version = ELC_USB_VERSION;
  uint8_t node = 0xFF;  // 0xFF until the board's CAN node is set
  ElcCanNodeType type = ElcCanNodeType::Unknown;
  ElcUsbMode mode = ElcUsbMode::Peer;
  uint32_t baud = 0;    // the rate the board is on after this record
  bool can = false;     // the CAN controller runs
  uint8_t fw = 0;
};

uint16_t elc_usb_crc16(const uint8_t *p, size_t n);

/** Wraps a payload: 0x00, COBS(payload + CRC), 0x00. Returns the bytes written (0 when it does not fit). */
size_t elc_usb_wrap(const uint8_t *payload, size_t n, uint8_t *out, size_t cap);

size_t elc_usb_encode_frame(const ElcCanFrame &f, uint8_t *out, size_t cap);
size_t elc_usb_encode_keepalive(const ElcUsbKeepalive &k, uint8_t *out, size_t cap);
size_t elc_usb_encode_status(const ElcUsbStatus &s, uint8_t *out, size_t cap);

/** Decoders take a checked payload (ElcUsbParser::payload()). */
bool elc_usb_decode_frame(const uint8_t *p, size_t n, ElcCanFrame &f);
bool elc_usb_decode_keepalive(const uint8_t *p, size_t n, ElcUsbKeepalive &k);
bool elc_usb_decode_status(const uint8_t *p, size_t n, ElcUsbStatus &s);

/** Splits the incoming byte stream into console text and records. */
class ElcUsbParser {
 public:
  enum Event : uint8_t { None, Text, Record };
  /** One byte in. Text: *text is a console byte. Record: payload() holds a record whose CRC checked. */
  Event feed(uint8_t b, uint8_t *text);
  const uint8_t *payload() const { return _dec; }
  size_t payload_len() const { return _dn; }
  uint32_t bad() const { return _bad; }  // records dropped (CRC, COBS or length)

 private:
  bool _in = false;
  uint8_t _buf[ELC_USB_MAX_WIRE];
  size_t _n = 0;
  uint8_t _dec[ELC_USB_MAX_WIRE];
  size_t _dn = 0;
  uint32_t _bad = 0;
};

// ---- the board's end of the link (Serial). Inert on host builds.

struct ElcUsbStats {
  uint32_t frames_up = 0;    // frames sent to the relay
  uint32_t frames_down = 0;  // frames from the relay
  uint32_t dropped = 0;      // frames from the relay lost to a full queue
  uint32_t bad = 0;          // records that failed their check
};

/** Opens Serial with a receive buffer big enough for the fast rates. Call instead of Serial.begin(). */
void elc_usb_begin(uint32_t baud = ELC_USB_DEFAULT_BAUD);

/** Console text from the port: use these instead of Serial.available() / Serial.read(). */
int elc_usb_available();
int elc_usb_read();

/** A relay is talking to this board. */
bool elc_usb_active();
bool elc_usb_gateway();
const ElcUsbStats &elc_usb_stats();

/** Reads the port: records are handled, text is kept for elc_usb_read(). ElcCan::poll() and elc_usb_available()
 *  call it. */
void elc_usb_pump();

/** What the status record reports (ElcCan::begin sets it). */
void elc_usb_set_identity(uint8_t node, ElcCanNodeType type, uint8_t fw);
void elc_usb_set_can_running(bool running);

/** Sends a frame to the relay (only while active). */
bool elc_usb_send_frame(const ElcCanFrame &f);

/** A frame from the relay, oldest first. */
bool elc_usb_take_frame(ElcCanFrame &f);

/** One line for the bench consoles. */
void elc_usb_describe(char *out, size_t n);
