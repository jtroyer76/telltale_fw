// settings_ui.h — on the display (docs/SPEC.md §5.6): the Settings panel (status, the device
// password, Change WiFi, Restart), opened from the gear on every screen; and, while the setup
// hotspot is on, the setup screen with its QR codes. Both sit on LVGL's top layer, over the
// screens, so a config change doesn't remove them. Call with the LVGL lock held.
#pragma once

#include "lvgl.h"

// The gear button that opens Settings, at the top right of `parent`. Returns it, for layout.
lv_obj_t *settings_ui_add_gear(lv_obj_t *parent);

// Keeps both up to date; call from the screens' refresh timer.
void settings_ui_refresh(void);

// The Settings panel or setup screen if one is open (it covers the screens), else NULL.
lv_obj_t *settings_ui_overlay(void);
