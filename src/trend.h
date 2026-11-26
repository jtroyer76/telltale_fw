// trend.h — the recent past of a number, for the chart behind a `value` tile (docs/SPEC.md §6.7).
// A trend is a row of buckets, each the average of the values that arrived in it. Kept on the
// display; with a History API path in the config it is also filled from the server once.
// The bucket store has no ESP-IDF or LVGL in it, so it also builds for the host tests.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "config.h"

#define TREND_BUCKETS  120   // at most this many buckets in a window

typedef struct {
    int bucket_s;                    // length of a bucket
    int n;                           // buckets in the window, <= TREND_BUCKETS
    int64_t newest;                  // bucket number (epoch s / bucket_s) of the newest one held; -1 = none
    int64_t first_live;              // bucket number of the first live value; INT64_MAX = none yet
    float sum[TREND_BUCKETS];        // slot = bucket number % TREND_BUCKETS
    uint16_t count[TREND_BUCKETS];
} trend_t;

// An empty trend for a window of `minutes`. Buckets are whole seconds, so a window that doesn't
// divide into TREND_BUCKETS gets fewer, longer ones (5 minutes: 100 buckets of 3 s).
void trend_init(trend_t *t, int minutes);

// A live value at server time `ms` (epoch).
void trend_add(trend_t *t, int64_t ms, double v);

// A row from the server's history: kept only for buckets older than the first live value.
void trend_add_history(trend_t *t, int64_t ms, double v);

// The window ending at `now_ms`, oldest first: t->n averages in `out`, `valid` false where a
// bucket has no values (a gap). Returns how many are valid.
int trend_window(const trend_t *t, int64_t now_ms, float *out, bool *valid);

// The chart's scale for a window: its lowest and highest value with a little room, or the fixed
// ends from the config. False when there is nothing to scale (no valid bucket, or hi <= lo).
bool trend_scale(const float *vals, const bool *valid, int n, bool has_min, double min, bool has_max, double max,
                 float *lo, float *hi);

#ifdef ESP_PLATFORM
// ---- device: the trends of the running config ----

typedef struct {
    int n;                       // buckets, oldest first
    float v[TREND_BUCKETS];
    bool ok[TREND_BUCKETS];
    int valid;                   // how many are ok
    // what the window was built from; trends_window uses them to tell when it has changed
    unsigned seq;
    int64_t bucket;
} trend_window_t;

// Around a rebuild of the screens: trends_begin, a trends_open per trend, trends_end. A trend
// whose path and window are in the new config too keeps its data.
void trends_begin(void);
// Returns a handle, or -1 (none free). `history` is the History API values endpoint, "" = none.
int trends_open(const char *path, int minutes, const char *history);
void trends_end(void);

// Takes new values from the store into their buckets. Call on every refresh, whatever screen
// is on show.
void trends_poll(void);

// Fills `w` when the trend has changed since `w` was last filled (a bucket closed, or history
// arrived). False if nothing changed, or there is no clock yet. An angle's window is always empty.
bool trends_window(int handle, trend_window_t *w);

// For GET /api/status. False when `i` is past the last trend.
bool trends_status(int i, char *path, int path_len, int *minutes, int *points, const char **history);
#endif
