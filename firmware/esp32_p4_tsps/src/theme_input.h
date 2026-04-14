#pragma once

#include <stdbool.h>

void theme_input_begin();

// Last saved layout: true = classic 2 (four subgauges), false = classic 1 (fuel only).
bool theme_load_classic2();
void theme_save_classic2(bool classic2);

// Optional bench GPIO (short press) toggles classic layout when in Classic global theme.
void theme_poll_gpio_toggle(bool *toggled);
