#include <Arduino.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// Paul Stoffregen's Encoder uses AVR-style IO_REG_TYPE / PIN_TO_BASEREG; it does not build on
// Arduino Mbed RP2040 or Earle Philhower arduino-pico. Use polling ElcEncoder on all RP2040.
#if defined(ARDUINO_ARCH_MBED) || defined(ARDUINO_ARCH_RP2040)
// (no Encoder.h)
#else
#define ENCODER_DO_NOT_USE_INTERRUPTS
#include <Encoder.h>
#endif
#include <SPI.h>
#include <U8g2lib.h>

// USB CDC debug mirror (Pico / Mbed RP2040): boot, IR lines (IR proto=… addr=… cmd=…), heartbeat; Pi MD= not echoed.
// Set ELC_USB_DEBUG=0 before compile to disable. AVR builds ignore this (no separate USB console).
#if defined(ARDUINO_ARCH_RP2040) || defined(ARDUINO_ARCH_MBED)
#ifndef ELC_USB_DEBUG
#define ELC_USB_DEBUG 1
#endif
#else
#define ELC_USB_DEBUG 0
#endif

// Arduino-IRremote v4. Install: arduino-cli lib install "IRremote"
//
// USB IR log: raw pulse timings only (see pollInfrared); no protocol names on USB.
//
// Optional: #define ELC_IR_RECEIVE_RAW_ONLY before this block to disable almost all protocol
// decoders (only Denon runs — usually fails), so decode() returns UNKNOWN with raw buffer only.
// Tradeoff: NEC IR→Pi tokens in applyNecCommand() will not match. Omit for normal NEC remotes.
#if defined(ELC_IR_RECEIVE_RAW_ONLY)
#define DECODE_DENON
#define EXCLUDE_EXOTIC_PROTOCOLS
#define EXCLUDE_UNIVERSAL_PROTOCOLS
#else
// Do not set DECODE_* here: IRProtocol.h enables all common decoders + HASH/DISTANCE_WIDTH.
#endif
#define NO_LED_RECEIVE_FEEDBACK_CODE
#define RAW_BUFFER_LENGTH 100
#include <IRremote.hpp>

// Panel: default SSD1322 256x64 (4-wire SPI). Only enable SSD1306 for a *128x64* SPI module.
// Using SSD1306 on a 256x64 SSD1322 looks like random snow / garbage, not a blank screen.
// #define ELC_OLED_SSD1306_128X64
//
// SSD1322 256x64: U8g2 has NHD vs ZJY init tables (many AliExpress / generic boards are ZJY).
// If snow persists after a slower SPI (see OLED_SPI_MAX_HZ), try:
// #define ELC_OLED_SSD1322_ZJY

// Dedicated ATmega328P: rotary/menu -> Pi UART, Pi downlink -> SPI OLED.
// Panel: SSD1322 256x64 (4-wire SPI). U8g2 page buffer fits ATmega328P SRAM.
// Uplink:   MD=<int>,AD=<int>,MP=<count>,AP=<count>[,IR=<token>]\n
//           IR tokens: volume_up, volume_down, theme_next, map_toggle, center_toggle,
//           tune_up, tune_down, dash_exit, menu_open, diagnostics_open (see applyNecCommand).
// Downlink: MODE=…|TITLE=…|L1=…|L2=…|WL=…|WT=…|ST=…|SA=…|PR=0–100|PE=…|PT=…|TM=…|AP=…\n
//
// If no valid Pi downlink for PI_SILENCE_MS, show a procedural Chevy bowtie (outline only).

// --- Encoder / buttons ---
static const uint8_t PIN_MENU_A = 2;
static const uint8_t PIN_MENU_B = 3;
static const uint8_t PIN_MENU_BTN = 4;
static const uint8_t PIN_AUX_A = 5;
static const uint8_t PIN_AUX_B = 6;
static const uint8_t PIN_AUX_BTN = 7;
static const uint8_t PIN_MENU_LEFT = 8;
static const uint8_t PIN_MENU_SELECT = 9;

#if defined(ARDUINO_ARCH_RP2040)
// Pi Pico wiring defaults:
// - UART1 TX=GP0, RX=GP1 (Pi link @115200)
// - SPI0 SCK=GP18, MOSI=GP19, MISO=GP16
static const uint8_t PIN_IR_RECEIVE = 22;
static const uint8_t OLED_CS = 17;
static const uint8_t OLED_DC = 20;
static const uint8_t OLED_RST = 21;
static const uint8_t UART_TX_PIN = 0;
static const uint8_t UART_RX_PIN = 1;
static const uint8_t OLED_SPI_SCK = 18;
static const uint8_t OLED_SPI_MOSI = 19;
static const uint8_t OLED_SPI_MISO = 16;
#define ELC_LINK_SERIAL Serial1
#else
// IR demodulator OUT (active-low bursts; module usually has weak pull-up). Analog pin as digital in.
static const uint8_t PIN_IR_RECEIVE = A0;
// --- SPI OLED (SSD1322 256x64). HW SPI: MOSI=11, SCK=13 ---
static const uint8_t OLED_CS = 10;
// DC must go to the module's D/C pin; do not swap with RST — wrong wiring shows snow/garbage.
static const uint8_t OLED_DC = A4;
static const uint8_t OLED_RST = A5;
#define ELC_LINK_SERIAL Serial
#endif

#if defined(ARDUINO_ARCH_MBED) || defined(ARDUINO_ARCH_RP2040)
// Same transition table as PJRC Encoder (polling); pin A = bit0, pin B = bit1 of state 0..3.
class ElcEncoder {
 public:
  ElcEncoder(uint8_t pinA, uint8_t pinB) : pinA_(pinA), pinB_(pinB), pos_(0), lastState_(0xFF) {}

  long read() {
    uint8_t s = (digitalRead(pinA_) ? 1u : 0u) | (digitalRead(pinB_) ? 2u : 0u);
    if (lastState_ == 0xFF) {
      lastState_ = s;
      return pos_;
    }
    if (s != lastState_) {
      static const int8_t table[] = {
          0, 1, -1, 0, -1, 0, 0, 1, 1, 0, 0, -1, 0, -1, 1, 0,
      };
      uint8_t idx = static_cast<uint8_t>((lastState_ << 2) | s);
      pos_ += table[idx];
      lastState_ = s;
    }
    return pos_;
  }

  void write(long v) {
    pos_ = v;
    lastState_ = 0xFF;
  }

 private:
  uint8_t pinA_;
  uint8_t pinB_;
  long pos_;
  uint8_t lastState_;
};
#endif

// U8g2 defaults SSD1322 to 10 MHz; long wires / breadboards show speckles/snow — lower first.
// If still noisy: try 250000UL, or fix wiring (short leads, solid GND).
#ifndef OLED_SPI_MAX_HZ
#define OLED_SPI_MAX_HZ 400000UL
#endif
// SSD1322 grayscale: max contrast can look noisy on some panels.
#ifndef OLED_CONTRAST
#define OLED_CONTRAST 200
#endif

static const unsigned long PI_SILENCE_MS = 3000;

// Page-buffered driver (low RAM vs full framebuffer)
#ifdef ELC_OLED_SSD1306_128X64
static U8G2_SSD1306_128X64_NONAME_1_4W_HW_SPI u8g2(U8G2_R0, OLED_CS, OLED_DC, OLED_RST);
static const uint16_t SCREEN_W = 128;
static const uint16_t SCREEN_H = 64;
#elif defined(ELC_OLED_SSD1322_ZJY)
static U8G2_SSD1322_ZJY_256X64_1_4W_HW_SPI u8g2(U8G2_R0, OLED_CS, OLED_DC, OLED_RST);
static const uint16_t SCREEN_W = 256;
static const uint16_t SCREEN_H = 64;
#else
static U8G2_SSD1322_NHD_256X64_1_4W_HW_SPI u8g2(U8G2_R0, OLED_CS, OLED_DC, OLED_RST);
static const uint16_t SCREEN_W = 256;
static const uint16_t SCREEN_H = 64;
#endif

static int16_t bowScaleX(int16_t x) {
#ifdef ELC_OLED_SSD1306_128X64
  return static_cast<int16_t>(x >> 1);
#else
  return x;
#endif
}

static const unsigned long BUTTON_DEBOUNCE_MS = 60;
static const unsigned long HEARTBEAT_MS = 400;
// Quadrature counts per one menu/aux step. PJRC/ElcEncoder advance ±1 per A/B edge; many
// mechanical detents yield ~2 counts per click, so 4 here feels like “two detents per step”.
// Use 4 if your encoder reliably gives four counts per detent and the menu moves twice per click.
static const long ROTARY_COUNTS_PER_STEP = 2;

#if defined(ARDUINO_ARCH_MBED) || defined(ARDUINO_ARCH_RP2040)
static ElcEncoder menuEncoder(PIN_MENU_A, PIN_MENU_B);
static ElcEncoder auxEncoder(PIN_AUX_A, PIN_AUX_B);
#else
static Encoder menuEncoder(PIN_MENU_A, PIN_MENU_B);
static Encoder auxEncoder(PIN_AUX_A, PIN_AUX_B);
#endif
static long lastMenuEncoderCount = 0;
static long lastAuxEncoderCount = 0;

static uint8_t lastMenuBtn = HIGH;
static uint8_t lastAuxBtn = HIGH;
static uint8_t lastMenuLeft = HIGH;
static uint8_t lastMenuSelect = HIGH;
static unsigned long lastMenuBtnMs = 0;
static unsigned long lastAuxBtnMs = 0;
static unsigned long lastMenuLeftMs = 0;
static unsigned long lastMenuSelectMs = 0;
static unsigned long lastTxMs = 0;

static char pendingIrToken[20] = "";
static char lastIrHuman[36] = "-";

static long menuPresses = 0;
static long auxPresses = 0;
static int pendingMenuDelta = 0;
static int pendingAuxDelta = 0;

// Pi -> OLED state
static char oledMode[8] = "drive";
static char oledTitle[32] = "";
static char oledL1[40] = "";
static char oledL2[40] = "";
static char oledWl[16] = "OUTSIDE";
static char oledWt[12] = "--";
static char oledSt[32] = "";
static char oledSa[32] = "";
static uint8_t oledPr = 0;
static char oledPe[10] = "0:00";
static char oledPt[10] = "0:00";
static char oledTm[10] = "12:00";
static char oledAp[4] = "AM";
// Vertical scroll menu (Pi sends M0–M2, MT, MO, MG when MODE=menu)
static char oledM0[32] = "";
static char oledM1[32] = "";
static char oledM2[32] = "";
static char oledV0[32] = "";
static char oledV1[32] = "";
static char oledV2[32] = "";
static uint16_t oledMT = 0;
static uint16_t oledMO = 0;
static uint16_t oledMG = 0;
static bool oledDirty = true;
static unsigned long lastPiFrameMs = 0;
static bool everReceivedPi = false;

static char rxLine[512];
static uint16_t rxLen = 0;

#if ELC_USB_DEBUG
// Philhower: (bool)Serial is false for all of setup() until a host opens CDC (DTR). Mirroring in setup → no USB text.
// Defer boot lines to loop() via usbDbgEmitBootIfPending(). Mbed: mirror in linkBoth (Serial ready earlier).
#if defined(ARDUINO_ARCH_RP2040) && !defined(ARDUINO_ARCH_MBED)
#define ELC_USB_DEFER_TO_LOOP 1
#else
#define ELC_USB_DEFER_TO_LOOP 0
#endif

// Philhower: only write USB when CDC has a host. IR runs in loop() — OK here.
#if defined(ARDUINO_ARCH_RP2040) && !defined(ARDUINO_ARCH_MBED)
static bool usbDbgReady() { return static_cast<bool>(Serial); }
#else
static bool usbDbgReady() { return true; }
#endif

#if ELC_USB_DEFER_TO_LOOP
static bool usbDbgBootEmitted = false;
static bool usbDbgOledOk = false;

static void usbDbgEmitBootIfPending() {
  if (usbDbgBootEmitted) {
    return;
  }
  if (!static_cast<bool>(Serial)) {
    return;
  }
  usbDbgBootEmitted = true;
  Serial.println(F("elcam encoder: boot"));
  Serial.print(F("OLED driver: "));
#if defined(ELC_OLED_SSD1306_128X64)
  Serial.println(F("SSD1306 128x64"));
#elif defined(ELC_OLED_SSD1322_ZJY)
  Serial.println(F("SSD1322 ZJY 256x64"));
#else
  Serial.println(F("SSD1322 NHD 256x64"));
#endif
  Serial.print(F("OLED SPI Hz: "));
  Serial.println(static_cast<unsigned long>(OLED_SPI_MAX_HZ));
  Serial.println(usbDbgOledOk ? F("OLED: begin() OK") : F("OLED: begin() FAILED (check wiring / driver)"));
  Serial.println(F("[usb] Pi MD= uplink not echoed; IR learn mirrored when USB up"));
}
#else
static void usbDbgEmitBootIfPending() {}
#endif

static void usbDbgBegin() {
  Serial.begin(115200);
  // Service TinyUSB so the device finishes attaching before heavy setup (helps IDE Serial Monitor / macOS cu).
  for (uint8_t i = 0; i < 40; i++) {
    delay(5);
    yield();
  }
}

static void linkBothPrint(const __FlashStringHelper *m) {
  ELC_LINK_SERIAL.print(m);
#if !ELC_USB_DEFER_TO_LOOP
  if (usbDbgReady()) {
    Serial.print(m);
  }
#endif
}
static void linkBothPrintLn(const __FlashStringHelper *m) {
  ELC_LINK_SERIAL.println(m);
#if !ELC_USB_DEFER_TO_LOOP
  if (usbDbgReady()) {
    Serial.println(m);
  }
#endif
}
static void linkBothPrintNumber(unsigned long v) {
  ELC_LINK_SERIAL.println(v);
#if !ELC_USB_DEFER_TO_LOOP
  if (usbDbgReady()) {
    Serial.println(v);
  }
#endif
}
#else
static void usbDbgBegin() {}
static void usbDbgEmitBootIfPending() {}

static void linkBothPrint(const __FlashStringHelper *m) { ELC_LINK_SERIAL.print(m); }
static void linkBothPrintLn(const __FlashStringHelper *m) { ELC_LINK_SERIAL.println(m); }
static void linkBothPrintNumber(unsigned long v) { ELC_LINK_SERIAL.println(v); }
#endif

static void markPiFrame() {
  lastPiFrameMs = millis();
  everReceivedPi = true;
}

static bool piLinkLive() {
  if (!everReceivedPi) {
    return false;
  }
  return (millis() - lastPiFrameMs) < PI_SILENCE_MS;
}

static void sanitizeAscii(char *s) {
  for (; *s; ++s) {
    unsigned char c = static_cast<unsigned char>(*s);
    if (c < 0x20 || c > 0x7e) {
      *s = ' ';
    }
  }
}

static void copyField(char *dest, size_t destSz, const char *src) {
  if (!dest || destSz == 0) {
    return;
  }
  strncpy(dest, src ? src : "", destSz - 1);
  dest[destSz - 1] = 0;
  sanitizeAscii(dest);
}

static void parseDownlinkLine(char *line) {
  oledMT = 0;
  oledMO = 0;
  oledMG = 0;
  oledM0[0] = '\0';
  oledM1[0] = '\0';
  oledM2[0] = '\0';
  oledV0[0] = '\0';
  oledV1[0] = '\0';
  oledV2[0] = '\0';
  char *cursor = line;
  while (cursor && *cursor) {
    char *next = strchr(cursor, '|');
    if (next) {
      *next++ = 0;
    }
    char *eq = strchr(cursor, '=');
    if (eq) {
      *eq++ = 0;
      if (!strcmp(cursor, "MODE")) {
        copyField(oledMode, sizeof(oledMode), eq);
      } else if (!strcmp(cursor, "TITLE")) {
        copyField(oledTitle, sizeof(oledTitle), eq);
      } else if (!strcmp(cursor, "L1")) {
        copyField(oledL1, sizeof(oledL1), eq);
      } else if (!strcmp(cursor, "L2")) {
        copyField(oledL2, sizeof(oledL2), eq);
      } else if (!strcmp(cursor, "WL")) {
        copyField(oledWl, sizeof(oledWl), eq);
      } else if (!strcmp(cursor, "WT")) {
        copyField(oledWt, sizeof(oledWt), eq);
      } else if (!strcmp(cursor, "ST")) {
        copyField(oledSt, sizeof(oledSt), eq);
      } else if (!strcmp(cursor, "SA")) {
        copyField(oledSa, sizeof(oledSa), eq);
      } else if (!strcmp(cursor, "PR")) {
        unsigned long v = strtoul(eq, nullptr, 10);
        oledPr = v > 100 ? 100 : static_cast<uint8_t>(v);
      } else if (!strcmp(cursor, "PE")) {
        copyField(oledPe, sizeof(oledPe), eq);
      } else if (!strcmp(cursor, "PT")) {
        copyField(oledPt, sizeof(oledPt), eq);
      } else if (!strcmp(cursor, "TM")) {
        copyField(oledTm, sizeof(oledTm), eq);
      } else if (!strcmp(cursor, "AP")) {
        copyField(oledAp, sizeof(oledAp), eq);
      } else if (!strcmp(cursor, "M0")) {
        copyField(oledM0, sizeof(oledM0), eq);
      } else if (!strcmp(cursor, "M1")) {
        copyField(oledM1, sizeof(oledM1), eq);
      } else if (!strcmp(cursor, "M2")) {
        copyField(oledM2, sizeof(oledM2), eq);
      } else if (!strcmp(cursor, "V0")) {
        copyField(oledV0, sizeof(oledV0), eq);
      } else if (!strcmp(cursor, "V1")) {
        copyField(oledV1, sizeof(oledV1), eq);
      } else if (!strcmp(cursor, "V2")) {
        copyField(oledV2, sizeof(oledV2), eq);
      } else if (!strcmp(cursor, "MT")) {
        oledMT = static_cast<uint16_t>(strtoul(eq, nullptr, 10));
      } else if (!strcmp(cursor, "MO")) {
        oledMO = static_cast<uint16_t>(strtoul(eq, nullptr, 10));
      } else if (!strcmp(cursor, "MG")) {
        oledMG = static_cast<uint16_t>(strtoul(eq, nullptr, 10));
      }
    }
    cursor = next;
  }
  markPiFrame();
  oledDirty = true;
}

static void pollDownlink() {
  while (ELC_LINK_SERIAL.available() > 0) {
    int raw = ELC_LINK_SERIAL.read();
    if (raw < 0) {
      break;
    }
    char c = static_cast<char>(raw);
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      rxLine[rxLen] = 0;
      rxLen = 0;
      if (rxLine[0] != 0 && strstr(rxLine, "MODE=") != nullptr && strstr(rxLine, "TITLE=") != nullptr &&
          strchr(rxLine, '|') != nullptr) {
        parseDownlinkLine(rxLine);
      }
      return;
    }
    if (rxLen < static_cast<int>(sizeof(rxLine)) - 1) {
      rxLine[rxLen++] = c;
    } else {
      rxLen = 0;
    }
  }
}

// --- Procedural bowtie (256x64), thick stroke from user geometry ---
static void thickEdge(U8G2 &u8, int x0, int y0, int x1, int y1) {
  u8.drawLine(x0, y0, x1, y1);
  u8.drawLine(x0 + 1, y0, x1 + 1, y1);
  u8.drawLine(x0, y0 + 1, x1, y1 + 1);
}

static void drawChevyBowtie(U8G2 &u8) {
  // Vertices (outer outline), closed — matches centered 256x64 layout.
  static const int16_t vx[12] = {15, 30, 103, 103, 153, 153, 241, 226, 153, 153, 103, 103};
  static const int16_t vy[12] = {45, 19, 19, 9, 9, 19, 19, 45, 45, 55, 55, 45};
  for (uint8_t i = 0; i < 12; i++) {
    uint8_t j = (i + 1) % 12;
    thickEdge(u8, bowScaleX(vx[i]), vy[i], bowScaleX(vx[j]), vy[j]);
  }
  // Second inner trace for a heavier outline (approx. inset)
  static const int16_t ix[12] = {22, 34, 107, 107, 149, 149, 234, 221, 149, 149, 107, 107};
  static const int16_t iy[12] = {41, 23, 23, 15, 15, 23, 23, 41, 41, 51, 51, 41};
  for (uint8_t i = 0; i < 12; i++) {
    uint8_t j = (i + 1) % 12;
    u8.drawLine(bowScaleX(ix[i]), iy[i], bowScaleX(ix[j]), iy[j]);
  }
}

static void drawIdleBowtieScreen(U8G2 &u8) {
  u8.setDrawColor(0);
  u8.drawBox(0, 0, SCREEN_W, SCREEN_H);
  u8.setDrawColor(1);
  drawChevyBowtie(u8);
}

static void drawThermometerIcon(U8G2 &u8, int16_t x, int16_t y) {
  u8.drawLine(x + 3, y + 2, x + 3, y + 14);
  u8.drawDisc(x + 3, y + 18, 4, U8G2_DRAW_ALL);
}

static void drawEighthNoteIcon(U8G2 &u8, int16_t x, int16_t y) {
  u8.drawDisc(x + 3, y + 5, 3, U8G2_DRAW_ALL);
  u8.drawDisc(x + 3, y + 14, 2, U8G2_DRAW_ALL);
  u8.drawBox(x + 6, y + 2, 2, 12);
  u8.drawLine(x + 8, y + 2, x + 14, y + 6);
}

static void drawSegProgress(U8G2 &u8, int16_t x, int16_t y, int16_t w, int16_t h, uint8_t pct) {
  const int gap = 1;
  const int segW = 2;
  int n = (w + gap) / (segW + gap);
  if (n < 1) {
    return;
  }
  int filled = (static_cast<int32_t>(n) * static_cast<int32_t>(pct)) / 100;
  for (int i = 0; i < n; i++) {
    if (i < filled) {
      u8.drawBox(x + i * (segW + gap), y, segW, h);
    }
  }
}

// Label left, optional value right (Pi sends V0–V2 with M0–M2); legacy: empty V = full-width M line.
static void drawMenuRowLine(U8G2 &u8, int textX, int baseline, int trackX, const char *lab, const char *val) {
  const int gap = 4;
  const int rightEdge = trackX - 4;
  if (!val || !val[0]) {
    char buf[32];
    copyField(buf, sizeof(buf), lab);
    while (strlen(buf) > 1 && textX + u8.getStrWidth(buf) > rightEdge) {
      buf[strlen(buf) - 1] = 0;
    }
    u8.drawStr(textX, baseline, buf);
    return;
  }
  int vw = u8.getStrWidth(val);
  int valX = rightEdge - vw;
  if (valX < textX + 4) {
    valX = textX + 4;
  }
  int labelMaxW = valX - gap - textX;
  if (labelMaxW < 4) {
    u8.drawStr(valX, baseline, val);
    return;
  }
  char labBuf[32];
  copyField(labBuf, sizeof(labBuf), lab);
  while (strlen(labBuf) > 1 && u8.getStrWidth(labBuf) > labelMaxW) {
    labBuf[strlen(labBuf) - 1] = 0;
  }
  u8.drawStr(textX, baseline, labBuf);
  u8.drawStr(valX, baseline, val);
}

// 256×64 vertical list: title bar + 3 rows + scrollbar (matches Pi menu_render_on_oled).
static void drawMenuScroll(U8G2 &u8) {
  u8.setDrawColor(0);
  u8.drawBox(0, 0, SCREEN_W, SCREEN_H);
  u8.setDrawColor(1);
  u8.setFont(u8g2_font_6x10_tf);
  u8.drawBox(0, 0, SCREEN_W, 14);
  u8.setDrawColor(0);
  u8.drawStr(4, 11, oledTitle[0] ? oledTitle : "Menu");
  u8.setDrawColor(1);
  u8.drawHLine(0, 15, SCREEN_W);

  const int rowH = 12;
  const int bodyTop = 16;
  const int textX = 16;
  const int trackX = 250;
  const int trackY = 17;
  const int trackW = 4;
  const int trackH = 46;
  const int maxRows = 3;

  for (int i = 0; i < maxRows; ++i) {
    const int idx = static_cast<int>(oledMO) + i;
    if (oledMT == 0 || idx >= static_cast<int>(oledMT)) {
      break;
    }
    const char *lab = (i == 0) ? oledM0 : ((i == 1) ? oledM1 : oledM2);
    const char *val = (i == 0) ? oledV0 : ((i == 1) ? oledV1 : oledV2);
    if (!lab) {
      lab = "";
    }
    if (!val) {
      val = "";
    }
    const int yTop = bodyTop + i * rowH;
    const int baseline = yTop + 10;
    if (static_cast<int>(oledMG) == idx) {
      u8.setDrawColor(1);
      u8.drawRFrame(0, yTop - 1, trackX - 2, rowH + 1, 2);
    }
    u8.setDrawColor(1);
    u8.drawDisc(6, yTop + rowH / 2, 5, U8G2_DRAW_ALL);
    u8.setDrawColor(0);
    if (lab[0] != 0) {
      char mark[2] = {static_cast<char>(toupper(static_cast<unsigned char>(lab[0]))), 0};
      u8.drawStr(3, baseline, mark);
    }
    u8.setDrawColor(1);
    drawMenuRowLine(u8, textX, baseline, trackX, lab, val);
  }

  u8.setDrawColor(1);
  u8.drawFrame(trackX, trackY, trackW, trackH);
  if (oledMT > maxRows) {
    int thumbH = (maxRows * trackH) / static_cast<int>(oledMT);
    if (thumbH < 4) {
      thumbH = 4;
    }
    const int maxFirst = static_cast<int>(oledMT) - maxRows;
    int ty = trackY + 1;
    if (maxFirst > 0) {
      ty += (static_cast<int>(oledMO) * (trackH - 2 - thumbH)) / maxFirst;
    }
    u8.drawBox(trackX + 1, ty, trackW - 2, thumbH);
  }
}

static void drawMenuScreenLegacy(U8G2 &u8) {
  u8.setDrawColor(0);
  u8.drawBox(0, 0, SCREEN_W, SCREEN_H);
  u8.setDrawColor(1);
  u8.setFont(u8g2_font_6x10_tf);
  u8.drawBox(0, 0, SCREEN_W, 14);
  u8.setDrawColor(0);
  u8.drawStr(6, 11, " MENU ");
  u8.setDrawColor(1);
  u8.drawHLine(0, 15, SCREEN_W);
  int ty = 26;
  u8.drawStr(0, ty, oledTitle[0] ? oledTitle : "-");
  ty += 12;
  u8.drawStr(0, ty, oledL1[0] ? oledL1 : "-");
  ty += 12;
  u8.drawStr(0, ty, oledL2[0] ? oledL2 : "-");
}

static void drawMenuScreen(U8G2 &u8) {
  if (oledMT > 0) {
    drawMenuScroll(u8);
    return;
  }
  drawMenuScreenLegacy(u8);
}

#ifdef ELC_OLED_SSD1306_128X64
static void drawDriveDashCompact(U8G2 &u8) {
  u8.setDrawColor(0);
  u8.drawBox(0, 0, SCREEN_W, SCREEN_H);
  u8.setDrawColor(1);
  u8.setFont(u8g2_font_6x10_tf);
  u8.drawStr(0, 10, oledSt[0] ? oledSt : "-");
  u8.drawStr(0, 22, oledSa[0] ? oledSa : "-");
  u8.drawStr(0, 34, oledWt[0] ? oledWt : "--");
  u8.drawStr(0, 46, oledTm[0] ? oledTm : "--:--");
  u8.drawStr(72, 46, oledAp[0] ? oledAp : "");
}
#endif

#ifndef ELC_OLED_SSD1306_128X64
static void drawDriveDashboard(U8G2 &u8) {
  u8.setDrawColor(0);
  u8.drawBox(0, 0, SCREEN_W, SCREEN_H);
  u8.setDrawColor(1);

  drawThermometerIcon(u8, 4, 14);
  u8.setFont(u8g2_font_6x10_tf);
  u8.drawStr(18, 12, oledWl[0] ? oledWl : "OUT");
  u8.setFont(u8g2_font_10x20_tf);
  u8.drawStr(18, 32, oledWt[0] ? oledWt : "--");

  drawEighthNoteIcon(u8, 58, 6);
  u8.setFont(u8g2_font_helvB08_tf);
  u8.drawStr(76, 20, oledSt[0] ? oledSt : "-");
  u8.setFont(u8g2_font_6x10_tf);
  u8.drawStr(58, 34, oledSa[0] ? oledSa : "-");
  u8.drawHLine(58, 40, 132);
  drawSegProgress(u8, 58, 44, 130, 6, oledPr);

  u8.setFont(u8g2_font_6x10_tf);
  u8.drawStr(58, 62, oledPe[0] ? oledPe : "0:00");
  char tail[16];
  snprintf(tail, sizeof(tail), "/ %s", oledPt[0] ? oledPt : "0:00");
  u8.setFont(u8g2_font_6x10_tf);
  int tw = u8.getStrWidth(tail);
  u8.drawStr(186 - tw, 62, tail);

  u8.setFont(u8g2_font_10x20_tf);
  const char *tm = oledTm[0] ? oledTm : "--:--";
  u8.drawStr(196, 28, tm);
  int cw = u8.getStrWidth(tm);
  u8.setFont(u8g2_font_6x10_tf);
  u8.drawStr(196 + cw + 2, 28, oledAp[0] ? oledAp : "");
}
#endif

static void drawPiScreen(U8G2 &u8) {
  const bool menu = (strcmp(oledMode, "menu") == 0);
  if (menu) {
    drawMenuScreen(u8);
    return;
  }
#ifdef ELC_OLED_SSD1306_128X64
  drawDriveDashCompact(u8);
#else
  drawDriveDashboard(u8);
#endif
}

static void pushDisplayFrame() {
  // Stop IR sample timer only — do NOT use global noInterrupts(): Encoder uses pin ISRs;
  // masking all interrupts made rotation rough and hurt double-tap timing.
  IrReceiver.stop();
  u8g2.firstPage();
  do {
    if (!piLinkLive()) {
      drawIdleBowtieScreen(u8g2);
    } else {
      drawPiScreen(u8g2);
    }
  } while (u8g2.nextPage());
  IrReceiver.start();

  if (piLinkLive()) {
    oledDirty = false;
  }
}

static void queuePiIrToken(const char *token) {
  if (!token || !token[0]) {
    return;
  }
  strncpy(pendingIrToken, token, sizeof(pendingIrToken) - 1);
  pendingIrToken[sizeof(pendingIrToken) - 1] = 0;
}

// NEC command -> Pi token or on-MCU rotary/press. Edit hex values to match your remote; unknown codes
// log as "IR_LEARN ..." on Serial when IR_LOG_UNKNOWN is 1.
#ifndef IR_LOG_UNKNOWN
#define IR_LOG_UNKNOWN 1
#endif

static bool applyNecCommand(uint16_t address, uint8_t command) {
  snprintf(lastIrHuman, sizeof(lastIrHuman), "nec:a=%u:c=0x%02X", static_cast<unsigned>(address), command);

  // --- Navigation on MCU (same counters as physical encoders/buttons) ---
  switch (command) {
    case 0x10:
      pendingMenuDelta -= 1;
      return true;
    case 0x11:
      pendingMenuDelta += 1;
      return true;
    case 0x12:
      menuPresses += 1;
      return true;
    case 0x13:
      pendingAuxDelta -= 1;
      return true;
    case 0x14:
      pendingAuxDelta += 1;
      return true;
    case 0x15:
      auxPresses += 1;
      return true;
    // --- Media / menu actions -> Pi (must match encoder_arduino_serial.py allowlist) ---
    case 0x20:
      queuePiIrToken("volume_up");
      return true;
    case 0x21:
      queuePiIrToken("volume_down");
      return true;
    case 0x22:
      queuePiIrToken("theme_next");
      return true;
    case 0x23:
      queuePiIrToken("map_toggle");
      return true;
    case 0x24:
      queuePiIrToken("center_toggle");
      return true;
    case 0x25:
      queuePiIrToken("tune_up");
      return true;
    case 0x26:
      queuePiIrToken("tune_down");
      return true;
    case 0x27:
      queuePiIrToken("dash_exit");
      return true;
    case 0x28:
      queuePiIrToken("menu_open");
      return true;
    case 0x29:
      queuePiIrToken("diagnostics_open");
      return true;
    default:
      break;
  }
#if IR_LOG_UNKNOWN
  ELC_LINK_SERIAL.print(F("IR_LEARN "));
  ELC_LINK_SERIAL.println(lastIrHuman);
#endif
  return false;
}

static bool pollInfrared() {
  if (!IrReceiver.decode()) {
    return false;
  }
  if (IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_REPEAT) {
    IrReceiver.resume();
    return false;
  }
#if ELC_USB_DEBUG
  if (usbDbgReady()) {
    IrReceiver.printIRResultRawFormatted(&Serial, true);
  }
#endif
  bool hit = false;
  if (IrReceiver.decodedIRData.protocol == NEC) {
    hit = applyNecCommand(IrReceiver.decodedIRData.address, IrReceiver.decodedIRData.command);
  }
#if IR_LOG_UNKNOWN
  else if (IrReceiver.decodedIRData.protocol != UNKNOWN) {
    snprintf(lastIrHuman, sizeof(lastIrHuman), "p=%u", static_cast<unsigned>(IrReceiver.decodedIRData.protocol));
    ELC_LINK_SERIAL.print(F("IR_LEARN proto="));
    ELC_LINK_SERIAL.println(lastIrHuman);
  }
#endif
  IrReceiver.resume();
  return hit;
}

// UART rotary fields: semantic UP/DOWN per detent (Pi maps to signed deltas). Zero uses MD=0,AD=0.
static void appendEncodedDelta(const char *key, int pendingDelta) {
  ELC_LINK_SERIAL.print(key);
  ELC_LINK_SERIAL.print('=');
  if (pendingDelta == 0) {
    ELC_LINK_SERIAL.print('0');
    return;
  }
  const int n = abs(pendingDelta);
  const bool up = pendingDelta > 0;
  for (int i = 0; i < n; i++) {
    if (i > 0) {
      ELC_LINK_SERIAL.print(',');
      ELC_LINK_SERIAL.print(key);
      ELC_LINK_SERIAL.print('=');
    }
    ELC_LINK_SERIAL.print(up ? F("UP") : F("DOWN"));
  }
}

void emitFrame() {
  appendEncodedDelta("MD", pendingMenuDelta);
  ELC_LINK_SERIAL.print(',');
  appendEncodedDelta("AD", pendingAuxDelta);
  ELC_LINK_SERIAL.print(F(",MP="));
  ELC_LINK_SERIAL.print(menuPresses);
  ELC_LINK_SERIAL.print(F(",AP="));
  ELC_LINK_SERIAL.print(auxPresses);
  if (pendingIrToken[0] != 0) {
    ELC_LINK_SERIAL.print(F(",IR="));
    ELC_LINK_SERIAL.print(pendingIrToken);
    pendingIrToken[0] = 0;
  }
  ELC_LINK_SERIAL.println();
  pendingMenuDelta = 0;
  pendingAuxDelta = 0;
  lastTxMs = millis();
}

template <typename EncT>
void updateRotary(EncT &encoder, long &lastCount, int &pendingDelta) {
  long currentCount = encoder.read();
  long diff = currentCount - lastCount;
  if (diff == 0) {
    return;
  }
  if ((diff > 64) || (diff < -64)) {
    lastCount = currentCount;
    return;
  }
  while (diff >= ROTARY_COUNTS_PER_STEP) {
    pendingDelta += 1;
    diff -= ROTARY_COUNTS_PER_STEP;
    lastCount += ROTARY_COUNTS_PER_STEP;
  }
  while (diff <= -ROTARY_COUNTS_PER_STEP) {
    pendingDelta -= 1;
    diff += ROTARY_COUNTS_PER_STEP;
    lastCount -= ROTARY_COUNTS_PER_STEP;
  }
  lastCount = currentCount - diff;
}

// pushDisplayFrame() can take tens of ms; quadrature counts still advance during SPI.
// Without a second read+emit, those steps wait until the next loop() (~one OLED frame late).
static void flushRotaryUartAfterOled() {
  updateRotary(menuEncoder, lastMenuEncoderCount, pendingMenuDelta);
  updateRotary(auxEncoder, lastAuxEncoderCount, pendingAuxDelta);
  if (pendingMenuDelta != 0 || pendingAuxDelta != 0) {
    emitFrame();
  }
}

static void pulseOledReset() {
  pinMode(OLED_RST, OUTPUT);
  digitalWrite(OLED_RST, LOW);
  delay(10);
  digitalWrite(OLED_RST, HIGH);
  delay(10);
}

void setup() {
#if defined(ARDUINO_ARCH_MBED)
  usbDbgBegin();
#elif defined(ARDUINO_ARCH_RP2040)
  usbDbgBegin();
  Serial1.setTX(UART_TX_PIN);
  Serial1.setRX(UART_RX_PIN);
#endif
  ELC_LINK_SERIAL.begin(115200);
  delay(80);
  linkBothPrintLn(F("elcam encoder: boot"));

  pinMode(PIN_MENU_A, INPUT_PULLUP);
  pinMode(PIN_MENU_B, INPUT_PULLUP);
  pinMode(PIN_MENU_BTN, INPUT_PULLUP);
  pinMode(PIN_AUX_A, INPUT_PULLUP);
  pinMode(PIN_AUX_B, INPUT_PULLUP);
  pinMode(PIN_AUX_BTN, INPUT_PULLUP);
  pinMode(PIN_MENU_LEFT, INPUT_PULLUP);
  pinMode(PIN_MENU_SELECT, INPUT_PULLUP);

  menuEncoder.write(0);
  auxEncoder.write(0);
  lastMenuEncoderCount = 0;
  lastAuxEncoderCount = 0;
  lastMenuBtn = digitalRead(PIN_MENU_BTN);
  lastAuxBtn = digitalRead(PIN_AUX_BTN);
  lastMenuLeft = digitalRead(PIN_MENU_LEFT);
  lastMenuSelect = digitalRead(PIN_MENU_SELECT);

#if !defined(ARDUINO_ARCH_MBED) && defined(ARDUINO_ARCH_RP2040)
  SPI.setSCK(OLED_SPI_SCK);
  SPI.setTX(OLED_SPI_MOSI);
  SPI.setRX(OLED_SPI_MISO);
#endif
  SPI.begin();
  pinMode(OLED_CS, OUTPUT);
  digitalWrite(OLED_CS, HIGH);
  pinMode(OLED_DC, OUTPUT);
  pulseOledReset();

  linkBothPrint(F("OLED driver: "));
#if defined(ELC_OLED_SSD1306_128X64)
  linkBothPrintLn(F("SSD1306 128x64"));
#elif defined(ELC_OLED_SSD1322_ZJY)
  linkBothPrintLn(F("SSD1322 ZJY 256x64"));
#else
  linkBothPrintLn(F("SSD1322 NHD 256x64"));
#endif
  linkBothPrint(F("OLED SPI Hz: "));
  linkBothPrintNumber(static_cast<unsigned long>(OLED_SPI_MAX_HZ));

  u8g2.setBusClock(OLED_SPI_MAX_HZ);
  const bool oledOk = u8g2.begin();
#if ELC_USB_DEBUG
#if ELC_USB_DEFER_TO_LOOP
  usbDbgOledOk = oledOk;
#endif
#endif
  if (!oledOk) {
    linkBothPrintLn(F("OLED: begin() FAILED (check wiring / driver)"));
  } else {
    linkBothPrintLn(F("OLED: begin() OK"));
  }
  u8g2.setContrast(OLED_CONTRAST);
  u8g2.setPowerSave(0);
  pushDisplayFrame();

  IrReceiver.begin(PIN_IR_RECEIVE, false);
  emitFrame();
}

void loop() {
#if ELC_USB_DEBUG
  usbDbgEmitBootIfPending();
  // Repeating line so you can confirm USB CDC TX without relying on boot text alone (115200).
  static unsigned long usbHeartbeatMs = 0;
  if (usbDbgReady()) {
    const unsigned long t = millis();
    if (t - usbHeartbeatMs >= 2000UL) {
      usbHeartbeatMs = t;
      Serial.println(F("[usb] heartbeat"));
    }
  }
#endif
  pollDownlink();

  bool changed = false;
  unsigned long now = millis();

  if (pollInfrared()) {
    changed = true;
  }

  updateRotary(menuEncoder, lastMenuEncoderCount, pendingMenuDelta);
  updateRotary(auxEncoder, lastAuxEncoderCount, pendingAuxDelta);
  if (pendingMenuDelta != 0 || pendingAuxDelta != 0) {
    changed = true;
  }

  uint8_t menuBtn = digitalRead(PIN_MENU_BTN);
  if (menuBtn == LOW && lastMenuBtn == HIGH && (now - lastMenuBtnMs) >= BUTTON_DEBOUNCE_MS) {
    menuPresses += 1;
    lastMenuBtnMs = now;
    changed = true;
  }
  lastMenuBtn = menuBtn;

  uint8_t auxBtn = digitalRead(PIN_AUX_BTN);
  if (auxBtn == LOW && lastAuxBtn == HIGH && (now - lastAuxBtnMs) >= BUTTON_DEBOUNCE_MS) {
    auxPresses += 1;
    lastAuxBtnMs = now;
    changed = true;
  }
  lastAuxBtn = auxBtn;

  uint8_t menuLeft = digitalRead(PIN_MENU_LEFT);
  if (menuLeft == LOW && lastMenuLeft == HIGH && (now - lastMenuLeftMs) >= BUTTON_DEBOUNCE_MS) {
    pendingMenuDelta -= 1;
    lastMenuLeftMs = now;
    changed = true;
  }
  lastMenuLeft = menuLeft;

  uint8_t menuSelect = digitalRead(PIN_MENU_SELECT);
  if (menuSelect == LOW && lastMenuSelect == HIGH && (now - lastMenuSelectMs) >= BUTTON_DEBOUNCE_MS) {
    menuPresses += 1;
    lastMenuSelectMs = now;
    changed = true;
  }
  lastMenuSelect = menuSelect;

  if (changed || (now - lastTxMs) >= HEARTBEAT_MS) {
    emitFrame();
  }

  static bool wasLive = false;
  const bool live = piLinkLive();

  if (live != wasLive) {
    wasLive = live;
    pushDisplayFrame();
  } else if (live) {
    if (oledDirty) {
      pushDisplayFrame();
    }
  } else {
    // Idle (no Pi link): loop never saw live!=wasLive when both start false — refresh periodically.
    static unsigned long lastIdlePaintMs = 0;
    if (now - lastIdlePaintMs >= 500UL) {
      lastIdlePaintMs = now;
      pushDisplayFrame();
    }
  }

  flushRotaryUartAfterOled();
}
