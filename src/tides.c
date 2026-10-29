// tides.c — parsing and lookups (host-tested), plus a fetch task on the device.

#include "tides.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "sk_time.h"

static void parse_header(const cJSON *root, tides_t *t)
{
    const cJSON *station = cJSON_GetObjectItemCaseSensitive(root, "station");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(station, "name");
    const cJSON *datum = cJSON_GetObjectItemCaseSensitive(root, "datum");
    if (cJSON_IsString(name)) snprintf(t->station, sizeof(t->station), "%s", name->valuestring);
    if (cJSON_IsString(datum)) snprintf(t->datum, sizeof(t->datum), "%s", datum->valuestring);
}

// The API's levels are metres; anything else would draw a wrong curve, so refuse it.
static bool metres(const cJSON *root)
{
    const cJSON *units = cJSON_GetObjectItemCaseSensitive(root, "units");
    return !units || (cJSON_IsString(units) && strcmp(units->valuestring, "meters") == 0);
}

bool tides_parse_extremes(const char *json, tides_t *t)
{
    cJSON *root = cJSON_Parse(json);
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(root, "extremes");
    bool ok = cJSON_IsArray(list) && metres(root);
    if (ok) {
        parse_header(root, t);
        t->n_ext = 0;
        const cJSON *e;
        cJSON_ArrayForEach(e, list) {
            if (t->n_ext >= TIDES_MAX_EXTREMES) break;
            const cJSON *time = cJSON_GetObjectItemCaseSensitive(e, "time");
            const cJSON *level = cJSON_GetObjectItemCaseSensitive(e, "level");
            const cJSON *high = cJSON_GetObjectItemCaseSensitive(e, "high");
            tide_extreme_t *x = &t->ext[t->n_ext];
            if (!cJSON_IsString(time) || !sk_time_parse(time->valuestring, &x->ms) || !cJSON_IsNumber(level)) continue;
            x->level = (float)level->valuedouble;
            x->high = cJSON_IsTrue(high);
            t->n_ext++;
        }
    }
    cJSON_Delete(root);
    return ok;
}

bool tides_parse_timeline(const char *json, tides_t *t)
{
    cJSON *root = cJSON_Parse(json);
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(root, "timeline");
    bool ok = cJSON_IsArray(list) && metres(root);
    if (ok) {
        parse_header(root, t);
        t->n_pts = 0;
        const cJSON *p;
        cJSON_ArrayForEach(p, list) {
            if (t->n_pts >= TIDES_MAX_POINTS) break;
            const cJSON *time = cJSON_GetObjectItemCaseSensitive(p, "time");
            const cJSON *level = cJSON_GetObjectItemCaseSensitive(p, "level");
            int64_t ms;
            if (!cJSON_IsString(time) || !sk_time_parse(time->valuestring, &ms) || !cJSON_IsNumber(level)) continue;
            if (t->n_pts && ms <= t->pt_ms[t->n_pts - 1]) continue;   // keep it in time order
            t->pt_ms[t->n_pts] = ms;
            t->pt_level[t->n_pts] = (float)level->valuedouble;
            t->n_pts++;
        }
        ok = t->n_pts >= 2;
    }
    cJSON_Delete(root);
    return ok;
}

bool tides_level_at(const tides_t *t, int64_t ms, float *level)
{
    if (t->n_pts < 2 || ms < t->pt_ms[0] || ms > t->pt_ms[t->n_pts - 1]) return false;
    int i = 1;
    while (i < t->n_pts - 1 && t->pt_ms[i] < ms) i++;
    float f = (float)(ms - t->pt_ms[i - 1]) / (float)(t->pt_ms[i] - t->pt_ms[i - 1]);
    *level = t->pt_level[i - 1] + f * (t->pt_level[i] - t->pt_level[i - 1]);
    return true;
}

int tides_lowest(const tides_t *t, int64_t from, int64_t to)
{
    int best = -1;
    for (int i = 0; i < t->n_ext; i++) {
        const tide_extreme_t *x = &t->ext[i];
        if (x->high || x->ms < from || x->ms > to) continue;
        if (best < 0 || x->level < t->ext[best].level) best = i;
    }
    return best;
}

#ifdef ESP_PLATFORM
// ---- device: fetch every 30 minutes while a tide widget exists ----

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sk_client.h"
#include "sk_http.h"
#include "store.h"

static const char *TAG = "tides";

#define REFRESH_MS    (30 * 60 * 1000)
#define RETRY_MS      (60 * 1000)
#define NOT_READY_MS  (5 * 1000)          // waiting for the Signal K connection and its clock
#define MAX_BODY      (48 * 1024)
#define BEHIND_MS     (60 * 60 * 1000LL)   // fetch from an hour ago, so the curve starts before now

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static tides_t *s_tides;     // PSRAM; the published copy
static tides_t *s_work;      // PSRAM; filled by the fetch, then swapped in
static char s_api[CFG_PATH_LEN];
static int s_hours;

typedef enum { FETCH_OK, FETCH_NOT_READY, FETCH_FAILED } fetch_result_t;

static fetch_result_t fetch(void)
{
    int64_t now;
    char api[CFG_PATH_LEN];
    int hours;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(api, sizeof(api), "%s", s_api);
    hours = s_hours;
    xSemaphoreGive(s_lock);
    if (!api[0] || hours <= 0 || !sk_client_connected() || !store_clock_ms(&now)) return FETCH_NOT_READY;

    char start[32], end[32], path[CFG_PATH_LEN + 96];
    sk_time_format_iso(now - BEHIND_MS, start, sizeof(start));
    sk_time_format_iso(now + (int64_t)(hours + 1) * 3600000LL, end, sizeof(end));

    memset(s_work, 0, sizeof(*s_work));
    snprintf(path, sizeof(path), "%s/timeline?start=%s&end=%s", api, start, end);
    char *body = sk_http_get(path, MAX_BODY, TAG);
    bool ok = body && tides_parse_timeline(body, s_work);
    free(body);
    if (ok) {
        snprintf(path, sizeof(path), "%s/extremes?start=%s&end=%s", api, start, end);
        body = sk_http_get(path, MAX_BODY, TAG);
        ok = body && tides_parse_extremes(body, s_work);
        free(body);
    }
    if (!ok) {
        ESP_LOGW(TAG, "fetch failed");
        return FETCH_FAILED;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_work->ok = true;
    s_work->seq = s_tides->seq + 1;
    tides_t *t = s_tides;
    s_tides = s_work;
    s_work = t;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "%s: %d points, %d highs/lows", s_tides->station, s_tides->n_pts, s_tides->n_ext);
    return FETCH_OK;
}

static void tides_task(void *arg)
{
    for (;;) {
        fetch_result_t r = fetch();
        uint32_t wait = r == FETCH_OK ? REFRESH_MS : r == FETCH_NOT_READY ? NOT_READY_MS : RETRY_MS;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait));   // a config change fetches at once
    }
}

void tides_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_tides = heap_caps_calloc(1, sizeof(tides_t), MALLOC_CAP_SPIRAM);
    s_work = heap_caps_calloc(1, sizeof(tides_t), MALLOC_CAP_SPIRAM);
}

void tides_configure(const char *api, int hours)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool changed = strcmp(s_api, api ? api : "") != 0 || s_hours != hours;
    snprintf(s_api, sizeof(s_api), "%s", api ? api : "");
    s_hours = api ? hours : 0;
    if (changed) s_tides->ok = false;
    xSemaphoreGive(s_lock);
    if (!s_task && s_api[0]) xTaskCreate(tides_task, "tides", 6144, NULL, 3, &s_task);
    if (s_task && changed) xTaskNotifyGive(s_task);
}

bool tides_get(tides_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(out, s_tides, sizeof(*out));
    xSemaphoreGive(s_lock);
    return out->ok;
}
#endif
