// trend.c — the bucket store (host-tested), plus the running config's trends on the device.

#include "trend.h"

#include <math.h>
#include <string.h>

void trend_init(trend_t *t, int minutes)
{
    memset(t, 0, sizeof(*t));
    int secs = minutes * 60;
    t->bucket_s = (secs + TREND_BUCKETS - 1) / TREND_BUCKETS;
    if (t->bucket_s < 1) t->bucket_s = 1;
    t->n = secs / t->bucket_s;
    if (t->n < 1) t->n = 1;
    t->newest = -1;
    t->first_live = INT64_MAX;
}

static int64_t bucket_of(const trend_t *t, int64_t ms) { return ms / 1000 / t->bucket_s; }

// Makes `b` the newest bucket, emptying the slots it and the skipped ones reuse.
static void advance(trend_t *t, int64_t b)
{
    if (t->newest < 0 || b - t->newest >= TREND_BUCKETS) {
        memset(t->sum, 0, sizeof(t->sum));
        memset(t->count, 0, sizeof(t->count));
    } else {
        for (int64_t i = t->newest + 1; i <= b; i++) {
            t->sum[i % TREND_BUCKETS] = 0;
            t->count[i % TREND_BUCKETS] = 0;
        }
    }
    t->newest = b;
}

static void put(trend_t *t, int64_t b, double v)
{
    if (b < 0 || !isfinite(v)) return;
    if (t->newest >= 0 && b <= t->newest - TREND_BUCKETS) return;   // older than anything held
    if (b > t->newest) advance(t, b);
    int slot = (int)(b % TREND_BUCKETS);
    if (t->count[slot] == UINT16_MAX) return;
    t->sum[slot] += (float)v;
    t->count[slot]++;
}

void trend_add(trend_t *t, int64_t ms, double v)
{
    int64_t b = bucket_of(t, ms);
    if (b < 0 || !isfinite(v)) return;
    if (t->first_live == INT64_MAX) t->first_live = b;
    put(t, b, v);
}

void trend_add_history(trend_t *t, int64_t ms, double v)
{
    int64_t b = bucket_of(t, ms);
    if (b >= t->first_live) return;   // live values own their buckets
    put(t, b, v);
}

int trend_window(const trend_t *t, int64_t now_ms, float *out, bool *valid)
{
    int64_t last = bucket_of(t, now_ms);
    int n = 0;
    for (int k = 0; k < t->n; k++) {
        int64_t b = last - (t->n - 1) + k;
        int slot = (int)(((b % TREND_BUCKETS) + TREND_BUCKETS) % TREND_BUCKETS);
        bool held = t->newest >= 0 && b >= 0 && b <= t->newest && b > t->newest - TREND_BUCKETS;
        valid[k] = held && t->count[slot] > 0;
        out[k] = valid[k] ? t->sum[slot] / t->count[slot] : 0;
        if (valid[k]) n++;
    }
    return n;
}

bool trend_scale(const float *vals, const bool *valid, int n, bool has_min, double min, bool has_max, double max,
                 float *lo, float *hi)
{
    bool any = false;
    float a = 0, b = 0;
    for (int i = 0; i < n; i++) {
        if (!valid[i]) continue;
        if (!any || vals[i] < a) a = vals[i];
        if (!any || vals[i] > b) b = vals[i];
        any = true;
    }
    if (!any) return false;
    // A little room above and below; a flat line sits in the middle rather than on an edge.
    float pad = (b - a) * 0.1f;
    if (pad <= 0) pad = fabsf(b) * 0.05f;
    if (pad <= 0) pad = 1;
    *lo = has_min ? (float)min : a - pad;
    *hi = has_max ? (float)max : b + pad;
    return *hi > *lo;
}

#ifdef ESP_PLATFORM
// ---- device ----

#include <stdio.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "history.h"
#include "sk_client.h"
#include "sk_http.h"
#include "store.h"

static const char *TAG = "trend";

#define MAX_BODY      (32 * 1024)   // TREND_BUCKETS rows are about 5 KB
#define MAX_ROWS      (TREND_BUCKETS * 2)
#define TRIES         5
#define RETRY_MS      (30 * 1000)
#define NOT_READY_MS  (5 * 1000)

typedef enum { HIST_NONE, HIST_PENDING, HIST_DONE, HIST_FAILED } hist_state_t;
static const char *const s_hist_names[] = { "none", "pending", "ok", "failed" };

typedef struct {
    bool used, keep;
    char path[CFG_PATH_LEN];
    int minutes;
    char url[CFG_PATH_LEN * 2 + 64];   // the history request
    hist_state_t hist;
    int tries;
    bool angle;                 // the value is an angle: not charted
    unsigned seq;               // bumped when history is merged or the trend is emptied
    int64_t last_us;            // the store's timestamp of the last value taken
    trend_t t;
} slot_t;

static SemaphoreHandle_t s_lock;
static slot_t *s_slots;        // PSRAM, CFG_MAX_TRENDS
static TaskHandle_t s_task;

static bool ready(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        s_slots = heap_caps_calloc(CFG_MAX_TRENDS, sizeof(slot_t), MALLOC_CAP_SPIRAM);
    }
    return s_lock && s_slots;
}

// One history request at a time, for the first trend still waiting for its rows.
static void fetch_task(void *arg)
{
    int64_t *ms = heap_caps_malloc(MAX_ROWS * sizeof(int64_t), MALLOC_CAP_SPIRAM);
    float *val = heap_caps_malloc(MAX_ROWS * sizeof(float), MALLOC_CAP_SPIRAM);
    for (;;) {
        char url[sizeof(s_slots[0].url)] = "";
        int i;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (i = 0; i < CFG_MAX_TRENDS; i++) {
            if (s_slots[i].used && s_slots[i].hist == HIST_PENDING) break;
        }
        if (i < CFG_MAX_TRENDS) snprintf(url, sizeof(url), "%s", s_slots[i].url);
        xSemaphoreGive(s_lock);
        if (!url[0] || !ms || !val) {   // nothing to do until the next config
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        int64_t now;
        if (!sk_client_connected() || !store_clock_ms(&now)) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(NOT_READY_MS));
            continue;
        }
        char *body = sk_http_get(url, MAX_BODY, TAG);
        int n = body ? history_parse_numbers(body, ms, val, MAX_ROWS) : -1;
        free(body);

        bool retry = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        slot_t *s = &s_slots[i];
        if (s->used && s->hist == HIST_PENDING && strcmp(s->url, url) == 0) {   // not replaced meanwhile
            if (n >= 0) {
                // An answer with no rows is an answer: the server has nothing for this path.
                for (int k = 0; k < n; k++) trend_add_history(&s->t, ms[k], val[k]);
                s->hist = HIST_DONE;
                s->seq++;
                ESP_LOGI(TAG, "%s: %d rows of history", s->path, n);
            } else if (++s->tries >= TRIES) {
                s->hist = HIST_FAILED;
                ESP_LOGW(TAG, "%s: no history; the trend starts from now", s->path);
            } else {
                retry = true;
            }
        }
        xSemaphoreGive(s_lock);
        if (retry) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(RETRY_MS));
    }
}

void trends_begin(void)
{
    if (!ready()) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < CFG_MAX_TRENDS; i++) s_slots[i].keep = false;
    xSemaphoreGive(s_lock);
}

int trends_open(const char *path, int minutes, const char *history)
{
    if (!ready()) return -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int found = -1, spare = -1;
    for (int i = 0; i < CFG_MAX_TRENDS; i++) {
        slot_t *s = &s_slots[i];
        if (s->used && s->minutes == minutes && strcmp(s->path, path) == 0) found = i;
        if (spare < 0 && !s->used) spare = i;
    }
    // None free: take one the new config hasn't asked for (trends_end would drop it anyway).
    for (int i = 0; i < CFG_MAX_TRENDS && found < 0 && spare < 0; i++) {
        if (!s_slots[i].keep) spare = i;
    }
    int i = found >= 0 ? found : spare;
    if (i >= 0) {
        slot_t *s = &s_slots[i];
        if (found < 0) {
            memset(s, 0, sizeof(*s));
            s->used = true;
            snprintf(s->path, sizeof(s->path), "%s", path);
            s->minutes = minutes;
            trend_init(&s->t, minutes);
        }
        s->keep = true;
        // Ask for history once per trend; again only if the last try failed.
        if (history[0] && (s->hist == HIST_NONE || s->hist == HIST_FAILED)) {
            snprintf(s->url, sizeof(s->url), "%s?paths=%s&duration=PT%dM&resolution=%d", history, path, minutes,
                     s->t.bucket_s);
            s->hist = HIST_PENDING;
            s->tries = 0;
        }
    }
    xSemaphoreGive(s_lock);
    return i;
}

void trends_end(void)
{
    if (!ready()) return;
    bool pending = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < CFG_MAX_TRENDS; i++) {
        slot_t *s = &s_slots[i];
        if (s->used && !s->keep) s->used = false;
        if (s->used && s->hist == HIST_PENDING) pending = true;
    }
    xSemaphoreGive(s_lock);
    if (!pending) return;
    if (!s_task) {
        xTaskCreate(fetch_task, "trend", 6144, NULL, 3, &s_task);
    } else {
        xTaskNotifyGive(s_task);
    }
}

void trends_poll(void)
{
    int64_t now;
    if (!s_slots || !store_clock_ms(&now)) return;
    int64_t t_us = esp_timer_get_time();
    for (int i = 0; i < CFG_MAX_TRENDS; i++) {
        char path[CFG_PATH_LEN];
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool used = s_slots[i].used;
        int64_t last_us = s_slots[i].last_us;
        if (used) snprintf(path, sizeof(path), "%s", s_slots[i].path);
        xSemaphoreGive(s_lock);
        if (!used) continue;

        store_value_t v;
        store_meta_t meta;
        if (!store_get(path, &v, &meta) || v.kind != VAL_NUMBER || !v.updated_us || v.updated_us == last_us) continue;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        slot_t *s = &s_slots[i];
        if (s->used && strcmp(s->path, path) == 0) {
            s->last_us = v.updated_us;
            // An average across north is wrong, and the line would jump at the wrap (SPEC §6.7).
            // The unit comes with the metadata, which can arrive after the first values: drop those.
            s->angle = strcmp(meta.si, "rad") == 0;
            if (!s->angle) {
                trend_add(&s->t, now - (t_us - v.updated_us) / 1000, v.number);
            } else if (s->t.newest >= 0) {
                trend_init(&s->t, s->minutes);
                s->seq++;
            }
        }
        xSemaphoreGive(s_lock);
    }
}

bool trends_window(int handle, trend_window_t *w)
{
    int64_t now;
    if (!s_slots || handle < 0 || handle >= CFG_MAX_TRENDS || !store_clock_ms(&now)) return false;
    bool changed = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const slot_t *s = &s_slots[handle];
    if (s->used) {
        int64_t bucket = now / 1000 / s->t.bucket_s;
        // seq + 1, so a window that was never filled (zeroed) always counts as changed
        if (w->seq != s->seq + 1 || w->bucket != bucket) {
            w->n = s->t.n;
            w->valid = trend_window(&s->t, now, w->v, w->ok);
            w->seq = s->seq + 1;
            w->bucket = bucket;
            changed = true;
        }
    }
    xSemaphoreGive(s_lock);
    return changed;
}

bool trends_status(int i, char *path, int path_len, int *minutes, int *points, const char **history)
{
    if (!s_slots) return false;
    int64_t now;
    bool have_clock = store_clock_ms(&now);
    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int k = 0; k < CFG_MAX_TRENDS && !found; k++) {
        const slot_t *s = &s_slots[k];
        if (!s->used || i-- > 0) continue;
        static float v[TREND_BUCKETS];
        static bool ok[TREND_BUCKETS];
        snprintf(path, path_len, "%s", s->path);
        *minutes = s->minutes;
        *points = have_clock ? trend_window(&s->t, now, v, ok) : 0;
        *history = s_hist_names[s->hist];
        found = true;
    }
    xSemaphoreGive(s_lock);
    return found;
}
#endif
