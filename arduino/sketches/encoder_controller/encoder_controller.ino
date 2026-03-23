#include <Arduino.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <Encoder.h>
#include <SPI.h>
#include <U8g2lib.h>

// NEC-only IR receive (Arduino-IRremote v4). Install: arduino-cli lib install "IRremote"
#define NO_LED_RECEIVE_FEEDBACK_CODE
#define RAW_BUFFER_LENGTH 100
#define DECODE_NEC
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

// IR demodulator OUT (active-low bursts; module usually has weak pull-up). Analog pin as digital in.
static const uint8_t PIN_IR_RECEIVE = A0;

// --- SPI OLED (SSD1322 256x64). HW SPI: MOSI=11, SCK=13 ---
static const uint8_t OLED_CS = 10;
// DC must go to the module's D/C pin; do not swap with RST — wrong wiring shows snow/garbage.
static const uint8_t OLED_DC = A4;
static const uint8_t OLED_RST = A5;

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
static const long ROTARY_COUNTS_PER_STEP = 4;

static Encoder menuEncoder(PIN_MENU_A, PIN_MENU_B);
static Encoder auxEncoder(PIN_AUX_A, PIN_AUX_B);
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
static bool oledDirty = true;
static unsigned long lastPiFrameMs = 0;
static bool everReceivedPi = false;

static char rxLine[256];
static uint8_t rxLen = 0;

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
      }
    }
    cursor = next;
  }
  markPiFrame();
  oledDirty = true;
}

static void pollDownlink() {
  while (Serial.available() > 0) {
    int raw = Serial.read();
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
    if (rxLen < sizeof(rxLine) - 1) {
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

static void drawMenuScreen(U8G2 &u8) {
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
  Serial.print(F("IR_LEARN "));
  Serial.println(lastIrHuman);
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
  bool hit = false;
  if (IrReceiver.decodedIRData.protocol == NEC) {
    hit = applyNecCommand(IrReceiver.decodedIRData.address, IrReceiver.decodedIRData.command);
  }
#if IR_LOG_UNKNOWN
  else if (IrReceiver.decodedIRData.protocol != UNKNOWN) {
    snprintf(lastIrHuman, sizeof(lastIrHuman), "p=%u", static_cast<unsigned>(IrReceiver.decodedIRData.protocol));
    Serial.print(F("IR_LEARN proto="));
    Serial.println(lastIrHuman);
  }
#endif
  IrReceiver.resume();
  return hit;
}

void emitFrame() {
  Serial.print(F("MD="));
  Serial.print(pendingMenuDelta);
  Serial.print(F(",AD="));
  Serial.print(pendingAuxDelta);
  Serial.print(F(",MP="));
  Serial.print(menuPresses);
  Serial.print(F(",AP="));
  Serial.print(auxPresses);
  if (pendingIrToken[0] != 0) {
    Serial.print(F(",IR="));
    Serial.print(pendingIrToken);
    pendingIrToken[0] = 0;
  }
  Serial.println();
  pendingMenuDelta = 0;
  pendingAuxDelta = 0;
  lastTxMs = millis();
}

void updateRotary(Encoder &encoder, long &lastCount, int &pendingDelta) {
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

static void pulseOledReset() {
  pinMode(OLED_RST, OUTPUT);
  digitalWrite(OLED_RST, LOW);
  delay(10);
  digitalWrite(OLED_RST, HIGH);
  delay(10);
}

void setup() {
  Serial.begin(115200);
  delay(80);
  Serial.println(F("elcam encoder: boot"));

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

  SPI.begin();
  pinMode(OLED_CS, OUTPUT);
  digitalWrite(OLED_CS, HIGH);
  pinMode(OLED_DC, OUTPUT);
  pulseOledReset();

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

  u8g2.setBusClock(OLED_SPI_MAX_HZ);
  if (!u8g2.begin()) {
    Serial.println(F("OLED: begin() FAILED (check wiring / driver)"));
  } else {
    Serial.println(F("OLED: begin() OK"));
  }
  u8g2.setContrast(OLED_CONTRAST);
  u8g2.setPowerSave(0);
  pushDisplayFrame();

  IrReceiver.begin(PIN_IR_RECEIVE, false);
  emitFrame();
}

void loop() {
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
}
