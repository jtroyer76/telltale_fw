// store.h — latest value and metadata per Signal K path. Written by the SK client, read by widgets.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "config.h"

#define STORE_MAX_PATHS  96
#define STORE_PATH_LEN   CFG_PATH_LEN
#define STORE_TEXT_LEN   32
#define STORE_BACKGROUND -1   // screen index for paths needed whatever screen is showing

typedef enum { VAL_NONE, VAL_NUMBER, VAL_TEXT, VAL_POSITION } val_kind_t;

typedef struct {
    val_kind_t kind;
    double number;
    char text[STORE_TEXT_LEN];
    double lat, lon;      // VAL_POSITION, degrees
    int64_t updated_us;   // esp_timer time the server got the value, 0 = never / unknown
} store_value_t;

// From Signal K metadata (stream sendMeta). Fields stay empty when the server has none.
typedef struct {
    bool has_meta;
    char si[CFG_UNIT_LEN];            // Signal K `units`, e.g. "K"
    char unit[CFG_UNIT_LEN];          // unit key from displayUnits (units_from_sk), "" = none
    int decimals;                     // from displayFormat, -1 = none
    char display_name[CFG_TEXT_LEN];
    int zone_count;
    zone_t zones[CFG_MAX_ZONES];
} store_meta_t;

void store_init(void);

// Forgets every path (before building screens from a new config).
void store_reset(void);

// Registers a path for `screen` (or STORE_BACKGROUND). Returns false when full.
bool store_watch(const char *path, int screen);
int store_watch_count(void);

// The screen on show; only its paths (and background ones) are subscribed.
void store_set_active_screen(int screen);
int store_active_screen(void);

// Calls `fn` for each path to subscribe to now, with the store locked (don't call back into
// the store from `fn`). Returns how many.
int store_for_each_subscribed(void (*fn)(const char *path, void *ctx), void *ctx);

void store_set_number(const char *path, double v, int64_t updated_us);
void store_set_text(const char *path, const char *s, int64_t updated_us);
void store_set_position(const char *path, double lat, double lon, int64_t updated_us);
// The server sent null (e.g. navigation.anchor.position when the anchor is raised): no value.
void store_set_null(const char *path, int64_t updated_us);
void store_set_meta(const char *path, const store_meta_t *meta);

// Copies the current value (and metadata if `meta` is not NULL); false if the path is not watched.
bool store_get(const char *path, store_value_t *out, store_meta_t *meta);

// Marks every value stale (used on reconnect).
void store_invalidate(void);

// The server's clock, from its hello message: epoch ms = esp_timer ms + offset_ms (SK client).
void store_set_clock(bool valid, int64_t offset_ms);

// Server time now (epoch ms, UTC). False until a server has said what time it is.
bool store_clock_ms(int64_t *ms);
