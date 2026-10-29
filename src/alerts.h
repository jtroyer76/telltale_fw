// alerts.h — active Signal K notifications (notifications.*) for alert widgets (docs/SPEC.md §6.3).
// Written by the SK client, read by widgets: like the store, widgets never see the WebSocket.
// No LVGL or ESP-IDF in the logic, so it also builds for the host tests.
#pragma once

#include <stdbool.h>
#include "config.h"

#define ALERTS_MAX        32
#define ALERTS_MSG_LEN    80
#define ALERTS_MAX_WATCH  16

typedef struct {
    char path[CFG_PATH_LEN];   // without the "notifications." prefix, e.g. "electrical.batteries.shunt.voltage"
    zone_state_t state;
    char message[ALERTS_MSG_LEN];
    unsigned seq;              // update order; newest wins a tie in severity
} alert_t;

void alerts_init(void);

// Forgets watches and alerts (before building screens from a new config).
void alerts_reset(void);

// An alert widget on `screen` watches `prefix` ("" = every notification). False when full.
bool alerts_watch(const char *prefix, int screen);

// Subscription patterns the screen on show needs: "notifications.<prefix>" and "notifications.<prefix>.*"
// (a wildcard doesn't match the path itself), or "notifications.*" for every notification.
int alerts_for_each_subscribed(int active_screen, void (*fn)(const char *path, void *ctx), void *ctx);

// From the stream. `path` without "notifications."; `state` a Signal K state name, NULL when the
// value was null (notification removed). normal/nominal count as not active.
void alerts_set(const char *path, const char *state, const char *message);

// Connection state. Not live: widgets say the alert state is unknown, never "no alerts".
// Going live clears the table; the server resends what is active on subscribe.
void alerts_set_live(bool live);
bool alerts_live(void);

// The most severe active alert (state alert or worse) under `prefix`, newest first on a tie;
// returns how many are active there (0: `out` untouched).
int alerts_worst(const char *prefix, alert_t *out);

// "" matches everything; otherwise the path itself or anything below it.
bool alerts_prefix_match(const char *prefix, const char *path);
