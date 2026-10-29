// config_store.h — where the running config comes from and how a new one is saved
// (docs/SPEC.md §6.1, §6.4). Order at boot: config.json, then config.lkg.json, then the
// built-in config. Saving keeps the config being replaced as config.lkg.json.
#pragma once

#include <stdbool.h>
#include "config.h"

#define CONFIG_PATH      "/data/config.json"
#define CONFIG_LKG_PATH  "/data/config.lkg.json"

typedef enum { CFG_SRC_SAVED, CFG_SRC_LKG, CFG_SRC_BUILTIN, CFG_SRC_NONE } config_source_t;

// Loads the boot config into `cfg`. `skip` > 0 skips that many sources from the front (crash-loop
// guard). `note` says why a fallback was used, "" if it wasn't. CFG_SRC_NONE: nothing loaded,
// `note` has the error.
config_source_t config_store_load(config_t *cfg, int skip, char *note, int note_len);

// Validates `json`; if good, saves it as config.json (the old one becomes config.lkg.json)
// and parses it into `cfg`. On failure nothing changes and `err` says why.
bool config_store_save(const char *json, config_t *cfg, char *err, int err_len);

// Deletes config.json and config.lkg.json, so the next boot uses the built-in config.
void config_store_reset(void);

// JSON text of the running config (caller frees), and where it came from.
char *config_store_running_json(void);
config_source_t config_store_running_source(void);
const char *config_source_name(config_source_t src);
