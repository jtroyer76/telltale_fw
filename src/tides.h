// tides.h — tide predictions for tide widgets (docs/SPEC.md §6.3), fetched over HTTP from a
// Signal K tides API (e.g. signalk-tides) every 30 minutes: the predicted level every few
// minutes (timeline) and the highs and lows (extremes). Levels in metres, as the API gives them.
// Parsing and lookups have no ESP-IDF in them, so they also build for the host tests.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "config.h"

#define TIDES_MAX_EXTREMES  16
#define TIDES_MAX_POINTS    360   // 10-minute points over 60 h

typedef struct {
    int64_t ms;      // epoch ms, UTC
    float level;     // metres above the datum
    bool high;
} tide_extreme_t;

typedef struct {
    bool ok;                  // predictions loaded
    unsigned seq;             // changes on every successful fetch
    char station[CFG_TEXT_LEN];
    char datum[12];           // e.g. "MLLW"
    int n_ext;
    tide_extreme_t ext[TIDES_MAX_EXTREMES];
    int n_pts;
    int64_t pt_ms[TIDES_MAX_POINTS];
    float pt_level[TIDES_MAX_POINTS];
} tides_t;

void tides_init(void);

// What the tide widgets need: the API base (e.g. "/signalk/v2/api/tides/stations/vessel/default",
// from config) and how many hours ahead. NULL / 0: no tide widget, nothing fetched.
void tides_configure(const char *api, int hours);

// Copies the latest predictions. False (and out->ok false) when there are none.
bool tides_get(tides_t *out);

// Parsers for the API's JSON; fill the matching part of `t`. False if the shape is wrong.
bool tides_parse_extremes(const char *json, tides_t *t);
bool tides_parse_timeline(const char *json, tides_t *t);

// Predicted level at `ms`, interpolated along the timeline. False outside it.
bool tides_level_at(const tides_t *t, int64_t ms, float *level);

// Index into t->ext of the lowest low in [from, to], or -1.
int tides_lowest(const tides_t *t, int64_t from, int64_t to);
