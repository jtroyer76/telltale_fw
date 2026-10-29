// app.h — what the API needs from main.c.
#pragma once

#include "config.h"

// Takes over `cfg` (PSRAM, from app_new_config) as the running config: rebuilds the screens,
// re-subscribes, switches Signal K server if it changed. The previous config is freed.
// Returns true if a restart is needed for everything to apply (hostname change).
bool app_apply_config(config_t *cfg);

// A zeroed config_t in PSRAM, or NULL.
config_t *app_new_config(void);
