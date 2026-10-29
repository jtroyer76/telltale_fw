#include "config_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "fs.h"

static const char *TAG = "config";

// The built-in config: first boot, and the last resort.
extern const char default_config_start[] asm("_binary_default_config_json_start");

static char *s_running_json;     // copy of what's running
static config_source_t s_running_src = CFG_SRC_NONE;
static SemaphoreHandle_t s_lock;

const char *config_source_name(config_source_t src)
{
    switch (src) {
    case CFG_SRC_SAVED:   return "saved";
    case CFG_SRC_LKG:     return "last-known-good";
    case CFG_SRC_BUILTIN: return "built-in";
    default:              return "none";
    }
}

static void set_running(const char *json, config_source_t src)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    char *copy = strdup(json);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    free(s_running_json);
    s_running_json = copy;
    s_running_src = src;
    xSemaphoreGive(s_lock);
}

static bool try_file(const char *path, config_t *cfg, char *err, int err_len)
{
    char *json = fs_read(path, CFG_MAX_JSON);
    if (!json) return false;
    bool ok = config_parse(json, cfg, err, err_len);
    if (ok) set_running(json, strcmp(path, CONFIG_PATH) == 0 ? CFG_SRC_SAVED : CFG_SRC_LKG);
    free(json);
    return ok;
}

config_source_t config_store_load(config_t *cfg, int skip, char *note, int note_len)
{
    char err[160] = "";
    note[0] = '\0';

    if (skip < 1) {
        if (try_file(CONFIG_PATH, cfg, err, sizeof(err))) return CFG_SRC_SAVED;
        if (fs_exists(CONFIG_PATH)) {
            ESP_LOGE(TAG, "config.json: %s", err);
            snprintf(note, note_len, "config.json rejected: %s", err);
        }
    } else {
        snprintf(note, note_len, "config.json skipped after repeated crashes");
    }

    if (skip < 2) {
        if (try_file(CONFIG_LKG_PATH, cfg, err, sizeof(err))) return CFG_SRC_LKG;
    }

    if (config_parse(default_config_start, cfg, err, sizeof(err))) {
        set_running(default_config_start, CFG_SRC_BUILTIN);
        return CFG_SRC_BUILTIN;
    }
    snprintf(note, note_len, "built-in config: %s", err);
    return CFG_SRC_NONE;
}

bool config_store_save(const char *json, config_t *cfg, char *err, int err_len)
{
    if (!config_parse(json, cfg, err, err_len)) return false;

    // The config being replaced becomes last-known-good, unless it is the built-in one
    // (always there anyway) or itself the LKG.
    if (s_running_src == CFG_SRC_SAVED && fs_exists(CONFIG_PATH) && !fs_rename(CONFIG_PATH, CONFIG_LKG_PATH)) {
        snprintf(err, err_len, "could not keep the current config as last-known-good");
        return false;
    }
    if (!fs_write_atomic(CONFIG_PATH, json, strlen(json))) {
        snprintf(err, err_len, "could not write config.json");
        return false;
    }
    set_running(json, CFG_SRC_SAVED);
    ESP_LOGI(TAG, "saved new config (%u bytes)", (unsigned)strlen(json));
    return true;
}

void config_store_reset(void)
{
    fs_remove(CONFIG_PATH);
    fs_remove(CONFIG_LKG_PATH);
}

char *config_store_running_json(void)
{
    if (!s_lock) return NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    char *copy = s_running_json ? strdup(s_running_json) : NULL;
    xSemaphoreGive(s_lock);
    return copy;
}

config_source_t config_store_running_source(void) { return s_running_src; }
