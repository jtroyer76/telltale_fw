#include "history.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sk_time.h"

int history_parse_positions(const char *json, track_pt_t *out, int max)
{
    const char *p = json ? strstr(json, "\"data\"") : NULL;
    if (!p || !(p = strchr(p, '['))) return 0;
    p++;   // inside the data array
    int n = 0;
    while (n < max) {
        while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r' || *p == '\t') p++;
        if (*p != '[') break;   // "]" ends the data array
        p++;
        while (*p == ' ') p++;
        if (*p != '"') break;
        const char *ts = ++p, *end = strchr(ts, '"');
        if (!end || end - ts > 39) break;
        char t[40];
        memcpy(t, ts, end - ts);
        t[end - ts] = '\0';
        p = end + 1;
        while (*p == ' ' || *p == ',') p++;
        int64_t ms;
        int depth = 1;   // inside the row
        if (*p == '[') {
            depth = 2;   // and inside its [lon, lat]
            char *q;
            double lon = strtod(p + 1, &q);
            while (*q == ' ' || *q == ',') q++;
            double lat = strtod(q, &q);
            p = q;
            if (sk_time_parse(t, &ms) && fabs(lat) <= 90 && fabs(lon) <= 180 && !(lat == 0 && lon == 0)) {
                out[n].lat_e7 = (int32_t)lround(lat * 1e7);
                out[n].lon_e7 = (int32_t)lround(lon * 1e7);
                out[n].ms = ms;
                if (n == 0 || ms > out[n - 1].ms) n++;   // keep time order
            }
        }
        // skip the rest of the row: past its closing bracket(s)
        while (*p && depth) {
            if (*p == '[') depth++;
            if (*p == ']') depth--;
            p++;
        }
        if (!*p) break;
    }
    return n;
}

#ifdef ESP_PLATFORM
// ---- device: one background fetch per request ----

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sk_client.h"
#include "sk_http.h"
#include "store.h"

static const char *TAG = "history";

#define RESOLUTION_S  30                 // a row every 30 s: 12 h is about 80 KB
#define MAX_BODY      (256 * 1024)
#define TRIES         5
#define RETRY_MS      (30 * 1000)
#define NOT_READY_MS  (5 * 1000)

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static char s_url[CFG_PATH_LEN * 2 + 64];   // the request, "" = none pending
static track_pt_t *s_pts;                  // PSRAM, TRACK_MAX
static int s_n;
static bool s_ready;

static void history_task(void *arg)
{
    for (int tries = 0;;) {
        char url[sizeof(s_url)];
        xSemaphoreTake(s_lock, portMAX_DELAY);
        snprintf(url, sizeof(url), "%s", s_url);
        xSemaphoreGive(s_lock);
        if (!url[0]) {   // nothing to do until the next request
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            tries = 0;
            continue;
        }
        int64_t now;
        if (!sk_client_connected() || !store_clock_ms(&now)) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(NOT_READY_MS));
            continue;
        }
        char *body = sk_http_get(url, MAX_BODY, TAG);
        int n = body ? history_parse_positions(body, s_pts, TRACK_MAX) : 0;
        free(body);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool same = strcmp(url, s_url) == 0;   // not replaced while we fetched
        if (same && (n > 0 || ++tries >= TRIES)) {
            s_n = n;
            s_ready = n > 0;
            s_url[0] = '\0';
        }
        xSemaphoreGive(s_lock);
        if (same && n > 0) ESP_LOGI(TAG, "%d positions", n);
        if (same && !n && tries < TRIES) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(RETRY_MS));
        if (same && !n && tries >= TRIES) ESP_LOGW(TAG, "gave up; the trail starts from now");
    }
}

void history_request(const char *api, const char *path, int minutes)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        s_pts = heap_caps_malloc(sizeof(track_pt_t) * TRACK_MAX, MALLOC_CAP_SPIRAM);
    }
    if (!s_pts) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_url, sizeof(s_url), "%s?paths=%s&duration=PT%dM&resolution=%d", api, path, minutes, RESOLUTION_S);
    s_ready = false;
    xSemaphoreGive(s_lock);
    if (!s_task) {
        xTaskCreate(history_task, "history", 6144, NULL, 3, &s_task);
    } else {
        xTaskNotifyGive(s_task);
    }
}

const track_pt_t *history_peek(int *n)
{
    if (!s_lock) return NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ready = s_ready;
    *n = s_n;
    xSemaphoreGive(s_lock);
    return ready ? s_pts : NULL;
}

void history_release(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ready = false;
    s_n = 0;
    xSemaphoreGive(s_lock);
}
#endif
