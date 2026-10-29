// theme.h — colours shared by screens and widgets. (Night mode, SPEC Backlog, will switch these.)
#pragma once

#include "lvgl.h"

#define COLOR_BG        lv_color_hex(0x0b1220)
#define COLOR_CARD      lv_color_hex(0x16213a)
#define COLOR_TEXT      lv_color_hex(0xe8edf5)
#define COLOR_MUTED     lv_color_hex(0x8a96ab)
#define COLOR_STALE     lv_color_hex(0x5a6478)
#define COLOR_OK        lv_color_hex(0x3ecf8e)
#define COLOR_WARN      lv_color_hex(0xf2b84b)
#define COLOR_ALARM     lv_color_hex(0xff5a5a)
#define COLOR_ACCENT    lv_color_hex(0x4ea1ff)   // gauge fill when the value is in no warning zone
#define COLOR_TRACK     lv_color_hex(0x34466e)   // gauge background (clearly lighter than COLOR_CARD)
