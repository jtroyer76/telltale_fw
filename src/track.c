#include "track.h"

#include <math.h>
#include <string.h>

#define M_PER_DEG_LAT  111320.0   // metres per degree of latitude (mean)
#define DEG_TO_RAD     (3.14159265358979323846 / 180.0)

void geo_offset_m(double lat0, double lon0, double lat, double lon, double *east, double *north)
{
    *north = (lat - lat0) * M_PER_DEG_LAT;
    *east = (lon - lon0) * M_PER_DEG_LAT * cos(lat0 * DEG_TO_RAD);
}

void track_reset(track_t *t)
{
    t->head = 0;
    t->count = 0;
}

const track_pt_t *track_at(const track_t *t, int i)
{
    int oldest = (t->head - t->count + TRACK_MAX) % TRACK_MAX;
    return &t->pts[(oldest + i) % TRACK_MAX];
}

bool track_add(track_t *t, double lat, double lon, int64_t ms)
{
    if (t->count) {
        const track_pt_t *last = track_at(t, t->count - 1);
        int64_t gap = ms - last->ms;
        if (gap < TRACK_MIN_GAP_MS) return false;
        double e, n;
        geo_offset_m(last->lat_e7 / 1e7, last->lon_e7 / 1e7, lat, lon, &e, &n);
        if (gap < TRACK_MAX_GAP_MS && e * e + n * n < TRACK_MIN_MOVE_M * TRACK_MIN_MOVE_M) return false;
    }
    track_pt_t *p = &t->pts[t->head];
    p->lat_e7 = (int32_t)lround(lat * 1e7);
    p->lon_e7 = (int32_t)lround(lon * 1e7);
    p->ms = ms;
    t->head = (t->head + 1) % TRACK_MAX;
    if (t->count < TRACK_MAX) t->count++;
    return true;
}

void track_backfill(track_t *t, const track_pt_t *older, int n, track_t *scratch)
{
    int64_t oldest = t->count ? track_at(t, 0)->ms : INT64_MAX;
    int keep = 0;
    while (keep < n && older[keep].ms < oldest) keep++;   // time order: the run before the live points
    int total = keep + t->count;
    int skip = total > TRACK_MAX ? total - TRACK_MAX : 0;
    track_reset(scratch);
    for (int i = 0; i < keep + t->count; i++) {
        if (i < skip) continue;
        scratch->pts[scratch->count++] = i < keep ? older[i] : *track_at(t, i - keep);
    }
    scratch->head = scratch->count % TRACK_MAX;
    memcpy(t, scratch, sizeof(*t));
}
