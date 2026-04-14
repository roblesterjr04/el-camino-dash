#pragma once

#include <stdint.h>

// Dedicated cluster I2C bus (second I2C peripheral). Default pins avoid display/touch on 7/8.
#ifndef CLUSTER_I2C_SDA
#define CLUSTER_I2C_SDA 47
#endif
#ifndef CLUSTER_I2C_SCL
#define CLUSTER_I2C_SCL 48
#endif

// 7-bit addresses — must not collide with touch / PMIC / audio on display bus.
static const uint8_t CLUSTER_I2C_ADDR_SPEEDO = 0x3C;
static const uint8_t CLUSTER_I2C_ADDR_TSPS = 0x3D;
static const uint8_t CLUSTER_I2C_ADDR_ESP32D_THEME = 0x3E;

static const uint8_t CLUSTER_REG_THEME = 0x01;
static const uint8_t CLUSTER_REG_CENTER_SUBVIEW = 0x02;
static const uint8_t CLUSTER_REG_TSPS_CLASSIC_VARIANT = 0x03;

enum class ClusterTheme : uint8_t { Classic = 0, Fallout = 1 };

enum class ClusterCenterSubview : uint8_t { Tach = 0, Clock = 1, Media = 2 };

enum class ClusterTspsClassicVariant : uint8_t { Classic1 = 0, Classic2 = 1 };

// NVS keys (Preferences namespace "cluster")
#define CLUSTER_NVS_NAMESPACE "cluster"
#define CLUSTER_NVS_KEY_THEME "th"
#define CLUSTER_NVS_KEY_CENTER_SUB "cv"
#define CLUSTER_NVS_KEY_TSPS_VAR "tv"

bool cluster_i2c_master_begin();
bool cluster_i2c_master_write_reg(uint8_t addr7, uint8_t reg, uint8_t value);

// Center is master: pushes theme + center sub-view to speedo/TSPS (and optional ESP32-D). Does not set TSPS
// classic layout — that stays local on the right gauge (3 s hold).
bool cluster_i2c_broadcast_center_theme(ClusterTheme theme, ClusterCenterSubview center_sub);
