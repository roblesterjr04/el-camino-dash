#pragma once

#include <Arduino.h>
#include <Preferences.h>

// USB serial bench shell: lines like "tz CST6CDT,M3.2.0,M11.1.0", "help"
void center_shell_poll(Preferences &prefs);
