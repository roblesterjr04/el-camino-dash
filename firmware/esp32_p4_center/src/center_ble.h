#pragma once

// ESP32-P4 (+ WiFi/BLE coprocessor): BLE GATT info line only. No Bluetooth Classic / A2DP
// phone audio on this hardware path. Advertising restarts when disconnected.
void center_ble_begin();
void center_ble_loop();
void center_ble_set_info_line(const char *utf8);
bool center_ble_is_connected();
