#pragma once

#include <Arduino.h>
#include <Preferences.h>

// Mount TF card (SDIO) when available and export NVS namespace "center" to the card.
void center_sd_settings_begin();

// True once the TF card mounted (the channel store keeps records on it).
bool center_sd_mounted();

// If elcamino/settings.json exists, load into NVS (call after begin, before anything reads prefs).
void center_sd_settings_import(Preferences &prefs);

// Write /sdcard/elcamino/settings.json (TZ, view, theme, screens). Safe to call often; no‑op if SD unavailable.
void center_sd_settings_sync(Preferences &prefs);
