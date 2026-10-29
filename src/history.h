// history.h — position history from the Signal K server's History API (v2), so the anchor view's
// trail survives restarts and updates (docs/SPEC.md §6.3). Fetched once, in the background, when
// an anchor widget is built; live positions take over from there.
// The parser has no ESP-IDF in it, so it also builds for the host tests.
#pragma once

#include "config.h"
#include "track.h"

// Reads the rows of a History API response for one position path: "data":[["<time>",[lon,lat]],...]
// (GeoJSON order: longitude first). Null rows are skipped. Points go into `out` in time order;
// returns how many. A small scanner rather than cJSON: 12 h of rows as a cJSON tree would need
// a few hundred KB of internal RAM.
int history_parse_positions(const char *json, track_pt_t *out, int max);

// Starts one fetch of the last `minutes` of `path` from `api` (e.g. /signalk/v2/api/history/values)
// once Signal K is connected and its clock known. A new request replaces an unfinished one.
void history_request(const char *api, const char *path, int minutes);

// The fetched points, once ready (NULL until then, and after history_release). Read them, then
// call history_release to free them.
const track_pt_t *history_peek(int *n);
void history_release(void);
