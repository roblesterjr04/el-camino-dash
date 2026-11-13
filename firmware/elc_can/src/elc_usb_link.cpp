#include "elc_usb_link.h"

#include <stdio.h>
#include <string.h>

#if defined(ARDUINO)
#include <Arduino.h>
#endif

// ---- records (host-testable)

uint16_t elc_usb_crc16(const uint8_t *p, size_t n) {
  uint16_t c = 0xFFFF;  // CRC-16/CCITT-FALSE (Python: binascii.crc_hqx(data, 0xFFFF))
  for (size_t i = 0; i < n; i++) {
    c ^= (uint16_t)p[i] << 8;
    for (int b = 0; b < 8; b++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
  }
  return c;
}

size_t elc_usb_wrap(const uint8_t *payload, size_t n, uint8_t *out, size_t cap) {
  if (n > ELC_USB_MAX_PAYLOAD || cap < n + 2 + 2 + 2) return 0;
  uint8_t raw[ELC_USB_MAX_PAYLOAD + 2];
  memcpy(raw, payload, n);
  const uint16_t crc = elc_usb_crc16(payload, n);
  raw[n] = (uint8_t)crc;
  raw[n + 1] = (uint8_t)(crc >> 8);
  const size_t rn = n + 2;
  // COBS: each block starts with the distance to the next zero (or to the end).
  size_t o = 0;
  out[o++] = 0x00;
  size_t code_at = o++;
  uint8_t code = 1;
  for (size_t i = 0; i < rn; i++) {
    if (raw[i] == 0) {
      out[code_at] = code;
      code_at = o++;
      code = 1;
    } else {
      out[o++] = raw[i];
      code++;
    }
  }
  out[code_at] = code;
  out[o++] = 0x00;
  return o;
}

size_t elc_usb_encode_frame(const ElcCanFrame &f, uint8_t *out, size_t cap) {
  uint8_t p[ELC_USB_MAX_PAYLOAD];
  const uint8_t len = f.len > 8 ? 8 : f.len;
  p[0] = ELC_USB_REC_FRAME;
  p[1] = (uint8_t)f.id;
  p[2] = (uint8_t)(f.id >> 8);
  p[3] = len;
  memcpy(p + 4, f.data, len);
  return elc_usb_wrap(p, 4u + len, out, cap);
}

size_t elc_usb_encode_keepalive(const ElcUsbKeepalive &k, uint8_t *out, size_t cap) {
  const uint8_t p[7] = {ELC_USB_REC_KEEPALIVE, k.version,          (uint8_t)k.mode,         (uint8_t)k.baud,
                        (uint8_t)(k.baud >> 8), (uint8_t)(k.baud >> 16), (uint8_t)(k.baud >> 24)};
  return elc_usb_wrap(p, sizeof(p), out, cap);
}

size_t elc_usb_encode_status(const ElcUsbStatus &s, uint8_t *out, size_t cap) {
  const uint8_t p[11] = {ELC_USB_REC_STATUS,     s.version,
                         s.node,                 (uint8_t)s.type,
                         (uint8_t)s.mode,        (uint8_t)s.baud,
                         (uint8_t)(s.baud >> 8), (uint8_t)(s.baud >> 16),
                         (uint8_t)(s.baud >> 24), (uint8_t)(s.can ? 1 : 0),
                         s.fw};
  return elc_usb_wrap(p, sizeof(p), out, cap);
}

static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool elc_usb_decode_frame(const uint8_t *p, size_t n, ElcCanFrame &f) {
  if (n < 4 || p[0] != ELC_USB_REC_FRAME || p[3] > 8 || n != 4u + p[3]) return false;
  f.id = (uint16_t)(p[1] | (p[2] << 8));
  if (f.id > 0x7FF) return false;
  f.len = p[3];
  memset(f.data, 0, sizeof(f.data));
  memcpy(f.data, p + 4, f.len);
  return true;
}

bool elc_usb_decode_keepalive(const uint8_t *p, size_t n, ElcUsbKeepalive &k) {
  if (n != 7 || p[0] != ELC_USB_REC_KEEPALIVE || p[1] == 0) return false;
  k.version = p[1];
  k.mode = p[2] == 1 ? ElcUsbMode::Gateway : ElcUsbMode::Peer;
  k.baud = rd32(p + 3);
  return true;
}

bool elc_usb_decode_status(const uint8_t *p, size_t n, ElcUsbStatus &s) {
  if (n != 11 || p[0] != ELC_USB_REC_STATUS) return false;
  s.version = p[1];
  s.node = p[2];
  s.type = (ElcCanNodeType)p[3];
  s.mode = p[4] == 1 ? ElcUsbMode::Gateway : ElcUsbMode::Peer;
  s.baud = rd32(p + 5);
  s.can = p[9] != 0;
  s.fw = p[10];
  return true;
}

ElcUsbParser::Event ElcUsbParser::feed(uint8_t b, uint8_t *text) {
  if (!_in) {
    if (b == 0x00) {
      _in = true;
      _n = 0;
      return None;
    }
    if (text) *text = b;
    return Text;
  }
  if (b != 0x00) {
    if (_n < sizeof(_buf)) {
      _buf[_n++] = b;
    } else {  // too long for a record: it was not one
      _bad++;
      _in = false;
      _n = 0;
    }
    return None;
  }
  if (_n == 0) return None;  // 0x00 0x00: the second one opens the record
  // COBS decode
  size_t i = 0;
  _dn = 0;
  bool ok = true;
  while (i < _n) {
    const uint8_t code = _buf[i++];
    if (code == 0 || i + code - 1 > _n) {
      ok = false;
      break;
    }
    for (uint8_t k = 1; k < code; k++) _dec[_dn++] = _buf[i++];
    if (code < 0xFF && i < _n) _dec[_dn++] = 0x00;
  }
  _n = 0;
  if (ok && _dn >= 3) {
    const uint16_t crc = (uint16_t)(_dec[_dn - 2] | (_dec[_dn - 1] << 8));
    if (elc_usb_crc16(_dec, _dn - 2) == crc) {
      _dn -= 2;
      _in = false;
      return Record;
    }
  }
  // A bad record: this 0x00 may have been the start of the next one (the stream slipped), so stay in.
  _bad++;
  return None;
}

// ---- the board's end

// A board that defines ELC_USB_PORT_EXTERNAL brings its own port (the sensor cards' card_usb.cpp: the AVR one, and
// the STM32 one, which stays at 115200); the AVR does so without the flag.
#if defined(ARDUINO) && !defined(ARDUINO_ARCH_AVR) && !defined(ELC_USB_PORT_EXTERNAL)

namespace {

#if ARDUINO_USB_CDC_ON_BOOT
constexpr bool kCanChangeBaud = false;  // native USB: there is no baud rate
#else
constexpr bool kCanChangeBaud = true;   // a USB-UART chip (CH343, CP210x) on UART0
#endif
// Album art streams at ~1250 frames/s (~16 kB/s on the wire): the queues ride out a slow loop pass (a face repaint,
// an OLED refresh) without dropping the 4 Hz transport frames in between.
#if CONFIG_IDF_TARGET_ESP32P4
constexpr size_t kRxBuffer = 32768;
constexpr size_t kFrameQueue = 1024;
#elif CONFIG_IDF_TARGET_ESP32S3
constexpr size_t kRxBuffer = 16384;
constexpr size_t kFrameQueue = 512;
#else  // the audio bridge: it receives little
constexpr size_t kRxBuffer = 8192;
constexpr size_t kFrameQueue = 128;
#endif

ElcUsbParser s_parser;
ElcUsbStats s_stats;
uint8_t s_text[256];
uint16_t s_text_h = 0, s_text_n = 0;
ElcCanFrame s_frames[kFrameQueue];
uint16_t s_fr_h = 0, s_fr_n = 0;
bool s_active = false;
ElcUsbMode s_mode = ElcUsbMode::Peer;
uint32_t s_last_ka = 0;
uint32_t s_baud = ELC_USB_DEFAULT_BAUD, s_base_baud = ELC_USB_DEFAULT_BAUD;
ElcUsbStatus s_id;
bool s_pumping = false;

bool baud_ok(uint32_t b) {
  return b == 115200 || b == 230400 || b == 460800 || b == 921600 || b == 1500000 || b == 2000000;
}

void set_baud(uint32_t b) {
  if (!kCanChangeBaud || b == s_baud) return;
  Serial.flush();
#if !ARDUINO_USB_CDC_ON_BOOT
  Serial.updateBaudRate(b);
#endif
  s_baud = b;
}

void reply(const ElcUsbKeepalive &k) {
  ElcUsbStatus st = s_id;
  st.mode = s_mode;
  const bool change = kCanChangeBaud && k.baud && k.baud != s_baud && baud_ok(k.baud);
  st.baud = change ? k.baud : (kCanChangeBaud ? s_baud : 0);
  uint8_t w[ELC_USB_MAX_WIRE];
  const size_t n = elc_usb_encode_status(st, w, sizeof(w));
  Serial.write(w, n);
  if (change) set_baud(k.baud);  // the reply goes out at the old rate, then both sides move
}

void record(const uint8_t *p, size_t n) {
  ElcCanFrame f;
  ElcUsbKeepalive k;
  if (elc_usb_decode_keepalive(p, n, k)) {
    s_last_ka = millis();
    s_active = true;
    s_mode = k.mode;
    reply(k);
  } else if (s_active && elc_usb_decode_frame(p, n, f)) {
    s_stats.frames_down++;
    if (s_fr_n == kFrameQueue) {  // full: drop the oldest
      s_fr_h = (uint16_t)((s_fr_h + 1) % kFrameQueue);
      s_fr_n--;
      s_stats.dropped++;
    }
    s_frames[(s_fr_h + s_fr_n) % kFrameQueue] = f;
    s_fr_n++;
  }
}

}  // namespace

void elc_usb_begin(uint32_t baud) {
  Serial.setRxBufferSize(kRxBuffer);
  Serial.begin(baud);
  s_baud = s_base_baud = baud;
}

void elc_usb_pump() {
  if (s_pumping) return;
  s_pumping = true;
  int budget = 4096;  // bound the work per call
  while (budget-- > 0 && Serial.available() > 0) {
    uint8_t t;
    const ElcUsbParser::Event e = s_parser.feed((uint8_t)Serial.read(), &t);
    if (e == ElcUsbParser::Text) {
      if (s_text_n < sizeof(s_text)) {
        s_text[(s_text_h + s_text_n) % sizeof(s_text)] = t;
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
    set_baud(s_base_baud);
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
  s_text_h = (uint16_t)((s_text_h + 1) % sizeof(s_text));
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
  if (!n || Serial.write(w, n) != n) return false;  // one write: records never split around other output
  s_stats.frames_up++;
  return true;
}

bool elc_usb_take_frame(ElcCanFrame &f) {
  if (!s_fr_n) return false;
  f = s_frames[s_fr_h];
  s_fr_h = (uint16_t)((s_fr_h + 1) % kFrameQueue);
  s_fr_n--;
  return true;
}

void elc_usb_describe(char *out, size_t n) {
  snprintf(out, n, "usb: %s%s, %lu baud, up %lu down %lu dropped %lu bad %lu", s_active ? "relay " : "off",
           s_active ? (s_mode == ElcUsbMode::Gateway ? "(gateway)" : "(peer)") : "", (unsigned long)s_baud,
           (unsigned long)s_stats.frames_up, (unsigned long)s_stats.frames_down, (unsigned long)s_stats.dropped,
           (unsigned long)s_stats.bad);
}

#elif !defined(ARDUINO_ARCH_AVR) && !defined(ELC_USB_PORT_EXTERNAL)  // host build: no port

static ElcUsbStats s_host_stats;
void elc_usb_begin(uint32_t) {}
int elc_usb_available() { return 0; }
int elc_usb_read() { return -1; }
bool elc_usb_active() { return false; }
bool elc_usb_gateway() { return false; }
const ElcUsbStats &elc_usb_stats() { return s_host_stats; }
void elc_usb_pump() {}
void elc_usb_set_identity(uint8_t, ElcCanNodeType, uint8_t) {}
void elc_usb_set_can_running(bool) {}
bool elc_usb_send_frame(const ElcCanFrame &) { return false; }
bool elc_usb_take_frame(ElcCanFrame &) { return false; }
void elc_usb_describe(char *out, size_t n) { snprintf(out, n, "usb: off"); }

#endif
