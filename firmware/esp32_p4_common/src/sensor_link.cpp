#include <sensor_link.h>

#include <Arduino_GFX_Library.h>

void SensorLink::begin(HardwareSerial &ser, int rx_pin, int tx_pin) {
  _ser = &ser;
  ser.setRxBufferSize(2048);
  ser.begin(115200, SERIAL_8N1, rx_pin, tx_pin);
  _parser.begin(ser);
}

bool SensorLink::poll() {
  if (!_ser) {
    return false;
  }
  bool got = false;
  SensorPacketValues v{};
  const uint32_t now = millis();
  // Drain everything buffered; keep the newest frame.
  while (_parser.poll(v, false)) {
    if (!_demo) {
      _held = v;
      got = true;
    }
  }
  if (_demo && (int32_t)(now - _demo_next_ms) >= 0) {
    _demo_next_ms = now + 50;
    _drive.frame(now, _held);
    got = true;
  }
  if (got) {
    _last_ms = now;
    _have = true;
  }
#if SENSOR_LINK_KEEPALIVE
  if (now - _last_keepalive_ms >= SENSOR_LINK_KEEPALIVE_MS) {
    _ser->write(PI_HANDSHAKE_BYTE);
    _last_keepalive_ms = now;
  }
#endif
  return got;
}

void SensorLink::setDemo(bool on) {
  if (on == _demo) {
    return;
  }
  _demo = on;
  if (on) {
    // Continue from the real odometer if we have one, so the numbers look plausible.
    _drive.start(millis(), (_have && _held.has_odometer) ? _held.odometer_tenths_mile : 966486u);
    _demo_next_ms = millis();
  } else {
    _have = false;  // show NO DATA until the next real frame instead of the last demo values
  }
}

bool SensorLink::fresh() const { return _have && (millis() - _last_ms) <= SENSOR_PACKET_STALE_MS; }

SensorDerived SensorLink::derived() const {
  if (fresh()) {
    return SensorPacketParser::derive(_held);
  }
  SensorDerived d{};
  d.valid = false;
  d.fuel_pct = 0.0f;
  d.oil_psi = 0.0f;
  d.coolant_temp_f = 100.0f;
  d.system_amp = 0.0f;
  return d;
}

bool SensorLink::odometerMiles(float *out) const {
  if (!fresh() || !_held.has_odometer) {
    return false;
  }
  if (out) {
    *out = (float)_held.odometer_tenths_mile * 0.1f;
  }
  return true;
}

static void render_no_data_text(Arduino_GFX *gfx, int16_t w, int16_t h, uint8_t *out_size, int16_t *out_x,
                                int16_t *out_y, uint16_t *out_w, uint16_t *out_h) {
  static const char kText[] = "NO DATA";
  const uint8_t ts = (w >= 600) ? 5 : 4;
  gfx->setFont(nullptr);
  gfx->setTextSize(ts);
  int16_t x1 = 0, y1 = 0;
  uint16_t tw = 0, th = 0;
  gfx->getTextBounds(kText, 0, 0, &x1, &y1, &tw, &th);
  *out_size = ts;
  *out_x = (int16_t)((w - (int16_t)tw) / 2 - x1);
  *out_y = (int16_t)((h * 7) / 10 - (int16_t)th / 2 - y1);
  *out_w = tw;
  *out_h = th;
}

void elc_fb_stamp_no_data(Arduino_GFX *gfx, uint16_t *fb, int cw) {
  if (!gfx || !fb || cw <= 0) {
    return;
  }
  // Dim to ~50% so the needles read as "at rest", not as real zero readings.
  const int n = cw * cw;
  for (int i = 0; i < n; i++) {
    fb[i] = (uint16_t)((fb[i] >> 1) & 0x7BEF);
  }

  static Arduino_Canvas *s_canvas = nullptr;
  static int s_cw = 0;
  static constexpr uint16_t kKey = 0x0821;  // transparent key (not produced by text rendering)
  const int16_t ch = (int16_t)(cw / 6);
  if (!s_canvas || s_cw != cw) {
    delete s_canvas;
    s_canvas = new Arduino_Canvas((int16_t)cw, ch, gfx, 0, 0, 0);
    if (!s_canvas->begin(GFX_SKIP_OUTPUT_BEGIN)) {
      delete s_canvas;
      s_canvas = nullptr;
      return;
    }
    s_cw = cw;
  }
  s_canvas->fillScreen(kKey);
  uint8_t ts;
  int16_t x, y;
  uint16_t tw, th;
  render_no_data_text(s_canvas, (int16_t)cw, ch, &ts, &x, &y, &tw, &th);
  y = (int16_t)((ch - (int16_t)th) / 2);
  s_canvas->setTextColor(RGB565(255, 40, 40));
  s_canvas->setCursor(x, y);
  s_canvas->print("NO DATA");

  const uint16_t *cb = s_canvas->getFramebuffer();
  const int dest_y = (cw * 7) / 10 - ch / 2;
  for (int yy = 0; yy < ch; yy++) {
    const int dy = dest_y + yy;
    if (dy < 0 || dy >= cw) {
      continue;
    }
    for (int xx = 0; xx < cw; xx++) {
      const uint16_t p = cb[yy * cw + xx];
      if (p != kKey) {
        fb[dy * cw + xx] = p;
      }
    }
  }
}

void elc_gfx_draw_no_data(Arduino_GFX *gfx, int16_t screen_w, int16_t screen_h) {
  if (!gfx) {
    return;
  }
  uint8_t ts;
  int16_t x, y;
  uint16_t tw, th;
  render_no_data_text(gfx, screen_w, screen_h, &ts, &x, &y, &tw, &th);
  gfx->fillRect((int16_t)(x - 8), (int16_t)(y - 8), (int16_t)(tw + 16), (int16_t)(th + 16), 0x0000);
  gfx->setTextColor(RGB565(255, 40, 40));
  gfx->setCursor(x, y);
  gfx->print("NO DATA");
}
