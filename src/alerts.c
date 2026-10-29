// alerts.c — fixed table, one mutex (none in the host tests). Linear search is fine at this size.

#include "alerts.h"

#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static SemaphoreHandle_t s_lock;
#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)
#else
#define LOCK()
#define UNLOCK()
#endif

typedef struct {
    char prefix[CFG_PATH_LEN];
    unsigned screens;   // bit per screen
} watch_t;

static alert_t s_alerts[ALERTS_MAX];
static int s_count;
static watch_t s_watches[ALERTS_MAX_WATCH];
static int s_watch_count;
static unsigned s_seq;
static volatile bool s_live;

void alerts_init(void)
{
#ifdef ESP_PLATFORM
    s_lock = xSemaphoreCreateMutex();
#endif
}

void alerts_reset(void)
{
    LOCK();
    s_count = 0;
    s_watch_count = 0;
    UNLOCK();
}

bool alerts_prefix_match(const char *prefix, const char *path)
{
    size_t n = strlen(prefix);
    return n == 0 || (strncmp(path, prefix, n) == 0 && (path[n] == '\0' || path[n] == '.'));
}

bool alerts_watch(const char *prefix, int screen)
{
    if (strlen(prefix) >= CFG_PATH_LEN) return false;
    LOCK();
    watch_t *w = NULL;
    for (int i = 0; i < s_watch_count; i++) {
        if (strcmp(s_watches[i].prefix, prefix) == 0) w = &s_watches[i];
    }
    if (!w && s_watch_count < ALERTS_MAX_WATCH) {
        w = &s_watches[s_watch_count++];
        snprintf(w->prefix, sizeof(w->prefix), "%s", prefix);
        w->screens = 0;
    }
    if (w) w->screens |= 1u << screen;
    UNLOCK();
    return w != NULL;
}

int alerts_for_each_subscribed(int active_screen, void (*fn)(const char *path, void *ctx), void *ctx)
{
    char pattern[CFG_PATH_LEN + 24];
    int n = 0;
    LOCK();
    for (int i = 0; i < s_watch_count; i++) {
        if (!(s_watches[i].screens & (1u << active_screen))) continue;
        if (s_watches[i].prefix[0]) {
            // Both the path itself and what's under it: hoekens-anchor-alarm raises its alarm at
            // notifications.navigation.anchor exactly, which "...anchor.*" doesn't match (checked on
            // Signal K 2.26.0, 2026-10-28).
            snprintf(pattern, sizeof(pattern), "notifications.%s", s_watches[i].prefix);
            fn(pattern, ctx);
            snprintf(pattern, sizeof(pattern), "notifications.%s.*", s_watches[i].prefix);
            fn(pattern, ctx);
            n += 2;
        } else {
            snprintf(pattern, sizeof(pattern), "notifications.*");
            fn(pattern, ctx);
            n++;
        }
    }
    UNLOCK();
    return n;
}

static int find(const char *path)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_alerts[i].path, path) == 0) return i;
    }
    return -1;
}

void alerts_set(const char *path, const char *state, const char *message)
{
    if (strlen(path) >= CFG_PATH_LEN) return;
    zone_state_t z = ZONE_NONE;
    if (state && !zone_state_from_name(state, &z)) z = ZONE_ALERT;   // unknown state: show it rather than hide it
    LOCK();
    int i = find(path);
    if (z < ZONE_ALERT) {   // cleared, normal or nominal
        if (i >= 0) s_alerts[i] = s_alerts[--s_count];
    } else {
        if (i < 0 && s_count < ALERTS_MAX) i = s_count++;
        if (i >= 0) {
            alert_t *a = &s_alerts[i];
            snprintf(a->path, sizeof(a->path), "%s", path);
            snprintf(a->message, sizeof(a->message), "%s", message ? message : "");
            a->state = z;
            a->seq = ++s_seq;
        }
    }
    UNLOCK();
}

void alerts_set_live(bool live)
{
    if (live && !s_live) {
        LOCK();
        s_count = 0;
        UNLOCK();
    }
    s_live = live;
}

bool alerts_live(void) { return s_live; }

int alerts_worst(const char *prefix, alert_t *out)
{
    int n = 0;
    const alert_t *best = NULL;
    LOCK();
    for (int i = 0; i < s_count; i++) {
        const alert_t *a = &s_alerts[i];
        if (!alerts_prefix_match(prefix, a->path)) continue;
        n++;
        if (!best || a->state > best->state || (a->state == best->state && a->seq > best->seq)) best = a;
    }
    if (best) *out = *best;
    UNLOCK();
    return n;
}
