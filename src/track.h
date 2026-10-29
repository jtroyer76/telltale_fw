// track.h — recent positions for the anchor view's swing trail (docs/SPEC.md §6.3), and the
// flat-earth geometry to draw them. Kept on the display, so the trail starts at boot.
// No ESP-IDF or LVGL in here, so it also builds for the host tests.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define TRACK_MAX           12000  // 16 h at one point every 5 s (a swinging boat); ~190 KB
#define TRACK_MIN_GAP_MS    5000   // keep a fix at most every 5 s...
#define TRACK_MIN_MOVE_M    0.5    // ...and only if the boat moved this far,
#define TRACK_MAX_GAP_MS    60000  // ...but at least one a minute

typedef struct {
    int32_t lat_e7, lon_e7;   // degrees x 1e7 (about 1 cm)
    int64_t ms;               // epoch ms
} track_pt_t;

typedef struct {
    track_pt_t pts[TRACK_MAX];
    int head;                 // next slot to write
    int count;
} track_t;

void track_reset(track_t *t);

// Adds a fix if it's worth keeping (see the limits above). True if kept.
bool track_add(track_t *t, double lat, double lon, int64_t ms);

// The i-th kept point, 0 = oldest.
const track_pt_t *track_at(const track_t *t, int i);

// East and north of (lat0, lon0) to (lat, lon), in metres. Flat-earth: fine over an anchorage.
void geo_offset_m(double lat0, double lon0, double lat, double lon, double *east, double *north);

// Puts `older` (time order) in front of the trail: only the points older than its oldest, and if
// the total doesn't fit, the oldest are dropped. `scratch` is working space (a whole track_t).
void track_backfill(track_t *t, const track_pt_t *older, int n, track_t *scratch);
