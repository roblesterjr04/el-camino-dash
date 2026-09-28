#pragma once

// Must match firmware/esp32_p4_common/include/cluster_i2c.h (center master → slaves).
#ifndef CLUSTER_THEME_I2C_ADDR
#define CLUSTER_THEME_I2C_ADDR 0x3E
#endif

#ifndef CLUSTER_REG_THEME
#define CLUSTER_REG_THEME 0x01
#endif

#ifndef CLUSTER_REG_CENTER_SUBVIEW
#define CLUSTER_REG_CENTER_SUBVIEW 0x02
#endif
