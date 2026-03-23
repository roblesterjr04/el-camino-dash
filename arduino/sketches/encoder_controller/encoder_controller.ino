#include <Arduino.h>
#include <Encoder.h>

// Dedicated ATmega328P rotary/menu controller for the dashboard.
// Emits newline-terminated frames like:
// MD=1,AD=0,MP=12,AP=3

static const uint8_t PIN_MENU_A = 2;
static const uint8_t PIN_MENU_B = 3;
static const uint8_t PIN_MENU_BTN = 4;
static const uint8_t PIN_AUX_A = 5;
static const uint8_t PIN_AUX_B = 6;
static const uint8_t PIN_AUX_BTN = 7;
static const uint8_t PIN_MENU_LEFT = 8;
static const uint8_t PIN_MENU_SELECT = 9;

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

static long menuPresses = 0;
static long auxPresses = 0;
static int pendingMenuDelta = 0;
static int pendingAuxDelta = 0;

void emitFrame() {
  Serial.print(F("MD="));
  Serial.print(pendingMenuDelta);
  Serial.print(F(",AD="));
  Serial.print(pendingAuxDelta);
  Serial.print(F(",MP="));
  Serial.print(menuPresses);
  Serial.print(F(",AP="));
  Serial.println(auxPresses);
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
    // If the library count jumps unexpectedly (startup/noise), resync baseline.
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
  // Carry sub-step remainder forward so half-step counts are preserved.
  lastCount = currentCount - diff;
}

void setup() {
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

  Serial.begin(115200);
  delay(10);
  emitFrame();
}

void loop() {
  bool changed = false;
  unsigned long now = millis();

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
}
