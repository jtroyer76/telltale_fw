// widgets.h — the widget registry's drawing side (docs/SPEC.md §5.4): create and update per
// widget type, keyed by the names in widget_types.c. Widgets read only the store.
#pragma once

#include "config.h"
#include "lvgl.h"

typedef struct {
    const widget_cfg_t *cfg;
    lv_obj_t *card;
    lv_obj_t *label;
    lv_obj_t *value;
    lv_obj_t *gauge;   // ring: the arc; bar: the bar; alert: the "+N more" line
    lv_obj_t *unit;    // value: the unit, small, in the bottom-right corner
    void *priv;        // type-specific state (tide), freed by widget_free
} widget_view_t;

// Builds the widget inside `card` (already sized and placed). Call with the LVGL lock held.
void widget_create(widget_view_t *v, lv_obj_t *card);

// Redraws from the store. Call with the LVGL lock held.
void widget_update(widget_view_t *v);

// Frees type-specific state, before the screens are rebuilt (LVGL objects go with the screen).
void widget_free(widget_view_t *v);

// Clock times (tide highs and lows) are shown in local time from this Signal K (-)hhmm offset
// path ("" = UTC, labelled so), 12 h or 24 h.
void widgets_set_clock(const char *offset_path, bool hour12);

// Every widget_types entry has drawing code; false (and logs) if not.
bool widgets_check_registry(void);

// Set a label's text / colour only when it changes. LVGL redraws on every set, even with the
// same value, and each redraw is PSRAM traffic the panel and WiFi compete for (SPEC §5.5).
void ui_set_text(lv_obj_t *label, const char *text);
void ui_set_text_color(lv_obj_t *obj, lv_color_t color);
