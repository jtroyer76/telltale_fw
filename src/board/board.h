// board.h — board profile interface. One implementation per board (board_lcd7.c, ...).
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#define BOARD_NAME   "Waveshare ESP32-S3-Touch-LCD-7"
#define BOARD_H_RES  800
#define BOARD_V_RES  480

// Brings up the panel, touch and LVGL. After this, LVGL calls must hold lvgl_port_lock().
esp_err_t board_init(void);

void board_backlight(bool on);
