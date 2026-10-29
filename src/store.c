// store.c — fixed table, one mutex. Small enough that a linear search is fine.

#include "store.h"

#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    char path[STORE_PATH_LEN];
    uint32_t screens;     // bit per screen that shows it
    bool background;
    store_value_t value;
    store_meta_t meta;
} entry_t;

static entry_t s_entries[STORE_MAX_PATHS];
static int s_count;
static int s_active;
static SemaphoreHandle_t s_lock;
static bool s_clock_valid;
static int64_t s_clock_offset_ms;

void store_init(void)
{
    s_lock = xSemaphoreCreateMutex();
}

static entry_t *find(const char *path)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_entries[i].path, path) == 0) return &s_entries[i];
    }
    return NULL;
}

void store_reset(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_count = 0;
    s_active = 0;
    xSemaphoreGive(s_lock);
}

bool store_watch(const char *path, int screen)
{
    if (strlen(path) >= STORE_PATH_LEN) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    entry_t *e = find(path);
    if (!e && s_count < STORE_MAX_PATHS) {
        e = &s_entries[s_count++];
        memset(e, 0, sizeof(*e));
        strcpy(e->path, path);
        e->meta.decimals = -1;
    }
    if (e) {
        if (screen == STORE_BACKGROUND) {
            e->background = true;
        } else {
            e->screens |= 1u << screen;
        }
    }
    xSemaphoreGive(s_lock);
    return e != NULL;
}

int store_watch_count(void) { return s_count; }

void store_set_active_screen(int screen)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_active = screen;
    xSemaphoreGive(s_lock);
}

int store_active_screen(void) { return s_active; }

int store_for_each_subscribed(void (*fn)(const char *path, void *ctx), void *ctx)
{
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        const entry_t *e = &s_entries[i];
        if (e->background || (e->screens & (1u << s_active))) {
            fn(e->path, ctx);
            n++;
        }
    }
    xSemaphoreGive(s_lock);
    return n;
}

void store_set_number(const char *path, double v, int64_t updated_us)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    entry_t *e = find(path);
    if (e) {
        e->value.kind = VAL_NUMBER;
        e->value.number = v;
        e->value.updated_us = updated_us;
    }
    xSemaphoreGive(s_lock);
}

void store_set_text(const char *path, const char *s, int64_t updated_us)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    entry_t *e = find(path);
    if (e) {
        e->value.kind = VAL_TEXT;
        strncpy(e->value.text, s, STORE_TEXT_LEN - 1);
        e->value.text[STORE_TEXT_LEN - 1] = '\0';
        e->value.updated_us = updated_us;
    }
    xSemaphoreGive(s_lock);
}

void store_set_position(const char *path, double lat, double lon, int64_t updated_us)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    entry_t *e = find(path);
    if (e) {
        e->value.kind = VAL_POSITION;
        e->value.lat = lat;
        e->value.lon = lon;
        e->value.updated_us = updated_us;
    }
    xSemaphoreGive(s_lock);
}

void store_set_null(const char *path, int64_t updated_us)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    entry_t *e = find(path);
    if (e) {
        e->value.kind = VAL_NONE;
        e->value.updated_us = updated_us;
    }
    xSemaphoreGive(s_lock);
}

void store_set_meta(const char *path, const store_meta_t *meta)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    entry_t *e = find(path);
    if (e) e->meta = *meta;
    xSemaphoreGive(s_lock);
}

bool store_get(const char *path, store_value_t *out, store_meta_t *meta)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    entry_t *e = find(path);
    if (e) {
        *out = e->value;
        if (meta) *meta = e->meta;
    }
    xSemaphoreGive(s_lock);
    return e != NULL;
}

void store_invalidate(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) s_entries[i].value.updated_us = 0;
    xSemaphoreGive(s_lock);
}

void store_set_clock(bool valid, int64_t offset_ms)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_clock_valid = valid;
    s_clock_offset_ms = offset_ms;
    xSemaphoreGive(s_lock);
}

bool store_clock_ms(int64_t *ms)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool valid = s_clock_valid;
    *ms = esp_timer_get_time() / 1000 + s_clock_offset_ms;
    xSemaphoreGive(s_lock);
    return valid;
}
