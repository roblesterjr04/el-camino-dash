// This card's end of the USB link (docs/protocols/usb_link.md) on USART1 (J7): the same elc_usb_* functions the ESP32
// boards have, the AVR card's version with the queues sized for 144 KB of RAM. Off until a relay (tools/can_relay.py)
// sends keepalives; until then the port is the console. Records and console text share the stream; only frames the
// card cares about (card_wants) are kept, because on the bench the relay copies every frame of every board to every
// board.
//
// The baud rate stays at 115200 and the status record says so, so the relay does not switch: the core's serial
// driver takes an interrupt per byte, and faster rates have not been tried on the card. (ELC_USB_PORT_EXTERNAL keeps
// elc_usb_link.cpp's ESP32 port, which switches rates, out of this build.)
#include <Arduino.h>

#include "card_net.h"
#include "elc_usb_link.h"

namespace {
const uint16_t kText = 128;
const uint16_t kFrames = 64;

ElcUsbParser s_parser;
ElcUsbStats s_stats;
uint8_t s_text[kText];
uint16_t s_text_h = 0, s_text_n = 0;
ElcCanFrame s_frames[kFrames];
uint16_t s_fr_h = 0, s_fr_n = 0;
bool s_active = false, s_pumping = false;
ElcUsbMode s_mode = ElcUsbMode::Peer;
uint32_t s_last_ka = 0;
uint32_t s_baud = ELC_USB_DEFAULT_BAUD;
ElcUsbStatus s_id;

void record(const uint8_t *p, size_t n) {
  ElcCanFrame f;
  ElcUsbKeepalive k;
  if (elc_usb_decode_keepalive(p, n, k)) {
    s_last_ka = millis();
    s_active = true;
    s_mode = k.mode;
    ElcUsbStatus st = s_id;
    st.mode = s_mode;
    st.baud = s_baud;  // the rate it is on: the relay follows that, and does not change
    uint8_t w[ELC_USB_MAX_WIRE];
    const size_t m = elc_usb_encode_status(st, w, sizeof(w));
    Serial.write(w, m);
  } else if (s_active && elc_usb_decode_frame(p, n, f)) {
    s_stats.frames_down++;
    if (!card_wants(f.id) && !elc_usb_gateway()) return;  // a gateway passes everything on to the bus
    if (s_fr_n == kFrames) {  // full: drop the oldest
      s_fr_h = (uint16_t)((s_fr_h + 1) % kFrames);
      s_fr_n--;
      s_stats.dropped++;
    }
    s_frames[(s_fr_h + s_fr_n) % kFrames] = f;
    s_fr_n++;
  }
}
}  // namespace

void elc_usb_begin(uint32_t baud) {
  Serial.begin(baud);
  s_baud = baud;
}

void elc_usb_pump() {
  if (s_pumping) return;
  s_pumping = true;
  int budget = 256;  // bound the work per call
  while (budget-- > 0 && Serial.available() > 0) {
    uint8_t t;
    const ElcUsbParser::Event e = s_parser.feed((uint8_t)Serial.read(), &t);
    if (e == ElcUsbParser::Text) {
      if (s_text_n < kText) {
        s_text[(s_text_h + s_text_n) % kText] = t;
        s_text_n++;
      }
    } else if (e == ElcUsbParser::Record) {
      record(s_parser.payload(), s_parser.payload_len());
    }
  }
  s_stats.bad = s_parser.bad();
  if (s_active && millis() - s_last_ka > ELC_USB_TIMEOUT_MS) {  // the relay went away
    s_active = false;
    s_mode = ElcUsbMode::Peer;
    s_fr_n = 0;
  }
  s_pumping = false;
}

int elc_usb_available() {
  elc_usb_pump();
  return s_text_n;
}

int elc_usb_read() {
  if (!s_text_n) elc_usb_pump();
  if (!s_text_n) return -1;
  const uint8_t c = s_text[s_text_h];
  s_text_h = (uint16_t)((s_text_h + 1) % kText);
  s_text_n--;
  return c;
}

bool elc_usb_active() { return s_active; }
bool elc_usb_gateway() { return s_active && s_mode == ElcUsbMode::Gateway; }
const ElcUsbStats &elc_usb_stats() { return s_stats; }

void elc_usb_set_identity(uint8_t node, ElcCanNodeType type, uint8_t fw) {
  s_id.node = node;
  s_id.type = type;
  s_id.fw = fw;
}
void elc_usb_set_can_running(bool running) { s_id.can = running; }

bool elc_usb_send_frame(const ElcCanFrame &f) {
  if (!s_active) return false;
  uint8_t w[ELC_USB_MAX_WIRE];
  const size_t n = elc_usb_encode_frame(f, w, sizeof(w));
  if (!n || Serial.write(w, n) != n) return false;  // one write: a record is never split around other output
  s_stats.frames_up++;
  return true;
}

bool elc_usb_take_frame(ElcCanFrame &f) {
  if (!s_fr_n) return false;
  f = s_frames[s_fr_h];
  s_fr_h = (uint16_t)((s_fr_h + 1) % kFrames);
  s_fr_n--;
  return true;
}

void elc_usb_describe(char *out, size_t n) {
  snprintf(out, n, "usb: %s%s, %lu baud, up %lu down %lu dropped %lu bad %lu", s_active ? "relay " : "off",
           s_active ? (s_mode == ElcUsbMode::Gateway ? "(gateway)" : "(peer)") : "", (unsigned long)s_baud,
           (unsigned long)s_stats.frames_up, (unsigned long)s_stats.frames_down, (unsigned long)s_stats.dropped,
           (unsigned long)s_stats.bad);
}
