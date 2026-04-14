#pragma once

#include <Arduino.h>
#include <Preferences.h>

// Mount TF card (SDIO) when available and export NVS namespace "center" to the card.
void center_sd_settings_begin();

// If elcamino/settings.json exists, load into NVS (call after begin, before Wi‑Fi/NTP uses prefs).
void center_sd_settings_import(Preferences &prefs);

// Write /sdcard/elcamino/settings.json (Wi‑Fi + TZ + view). Safe to call often; no‑op if SD unavailable.
void center_sd_settings_sync(Preferences &prefs);
