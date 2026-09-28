#pragma once

#include <stdint.h>

// Center views (menu or 3 s hold). Numeric values are persisted in NVS "view": keep Media = 2.
enum class CenterViewMode : uint8_t { Tach = 0, Clock = 1, Media = 2, Efficiency = 3 };
