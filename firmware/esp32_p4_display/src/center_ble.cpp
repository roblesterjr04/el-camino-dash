#include "center_ble.h"

#include <Arduino.h>
#include <string.h>

#if defined(ARDUINO_ARCH_ESP32) && __has_include(<BLEDevice.h>)

#include <BLEDevice.h>
#include <BLEServer.h>

static BLEServer *s_srv = nullptr;
static BLECharacteristic *s_chr = nullptr;
static char s_line[128];
static bool s_connected = false;
static uint32_t s_last_adv_ms = 0;

static const char *kSvc = "4fafc201-1fb5-459e-8fcc-c5c9c331914b";
static const char *kChr = "beb5483e-36e1-4688-b7f5-ea07361b26a8";
// Short GAP name: with a 128-bit service UUID in the same legacy ADV PDU, the name must
// stay small (~8 chars) or the stack drops fields; long names used scan-response-only and
// often do not show in the phone’s Bluetooth Settings (which targets Classic pairing anyway).
static const char *kGapName = "ElCamino";

class CenterBleCb : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    String v = c->getValue();
    if (v.length() == 0) {
      return;
    }
    size_t n = (size_t)v.length();
    if (n >= sizeof(s_line)) {
      n = sizeof(s_line) - 1;
    }
    memcpy(s_line, v.c_str(), n);
    s_line[n] = '\0';
  }
};

class CenterBleSrvCb : public BLEServerCallbacks {
  void onConnect(BLEServer *) override {
    s_connected = true;
  }
  void onDisconnect(BLEServer *) override {
    s_connected = false;
    BLEDevice::startAdvertising();
    s_last_adv_ms = millis();
  }
};

void center_ble_begin() {
  strncpy(s_line, "ElCamino center", sizeof(s_line) - 1);
  s_line[sizeof(s_line) - 1] = '\0';

  BLEDevice::init(kGapName);
  s_srv = BLEDevice::createServer();
  s_srv->setCallbacks(new CenterBleSrvCb());
  BLEService *svc = s_srv->createService(kSvc);
  s_chr = svc->createCharacteristic(kChr, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  s_chr->setCallbacks(new CenterBleCb());
  s_chr->setValue(s_line);
  svc->start();
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(kSvc);
  // Put the local name in the primary advertisement (not only scan response) so BLE scanners
  // show "ElCamino" without relying on active scan / scan response ordering.
  adv->setScanResponse(false);
  adv->setMinPreferred(0x06);
  adv->setMaxPreferred(0x12);
  BLEDevice::startAdvertising();
  s_last_adv_ms = millis();
}

void center_ble_loop() {
  const uint32_t now = millis();
  if (!s_connected && (now - s_last_adv_ms > 3000u)) {
    BLEDevice::startAdvertising();
    s_last_adv_ms = now;
  }
}

bool center_ble_is_connected() { return s_connected; }

void center_ble_set_info_line(const char *utf8) {
  if (!utf8) {
    return;
  }
  strncpy(s_line, utf8, sizeof(s_line) - 1);
  s_line[sizeof(s_line) - 1] = '\0';
  if (s_chr) {
    s_chr->setValue(s_line);
  }
}

#else

void center_ble_begin() { Serial.println("[ble] BLEDevice.h not available — skip BLE"); }

void center_ble_loop() {}

bool center_ble_is_connected() { return false; }

void center_ble_set_info_line(const char *) {}

#endif
