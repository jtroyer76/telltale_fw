// screens.h — builds LVGL screens from the config and refreshes them from the store.
// All functions: call with the LVGL lock held.
#pragma once

#include "config.h"

// Builds all screens (one swipeable tile each), replacing whatever was shown. Registers each
// widget's paths in the store (call store_reset first). `cfg` must outlive the screens.
void screens_build(const config_t *cfg);

// Full-screen error with the status line (IP), used when no config can be loaded.
void screens_show_error(const char *msg);

// Line at the bottom of every screen, e.g. "running last-known-good config". NULL or "" hides it.
void screens_set_notice(const char *msg);

// Shows screen `index` (as if swiped to). False if out of range.
bool screens_show(int index);

int screens_active(void);

// How drawing has gone since the last call: screen refreshes, their total and longest time, and
// the pixels drawn. For GET /api/status: a slow panel or build shows up here as numbers.
void screens_render_stats(int *frames, int *total_ms, int *max_ms, int *pixels_k);
