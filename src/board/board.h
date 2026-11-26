// board.h — board profile interface. One implementation per board (board_lcd7.c, board_crowpanel101.c),
// chosen at build time by CONFIG_TELLTALE_BOARD_* (src/Kconfig.projbuild), one PlatformIO env per board.
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "sdkconfig.h"

#if CONFIG_TELLTALE_BOARD_CROWPANEL101
#define BOARD_NAME   "Elecrow CrowPanel Advanced 10.1 ESP32-P4"
#define BOARD_H_RES  1024
#define BOARD_V_RES  600
#else
#define BOARD_NAME   "Waveshare ESP32-S3-Touch-LCD-7"
#define BOARD_H_RES  800
#define BOARD_V_RES  480
#endif

// Brings up the panel, touch and LVGL. After this, LVGL calls must hold lvgl_port_lock().
esp_err_t board_init(void);

void board_backlight(bool on);
