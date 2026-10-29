// config.h — the device config (docs/SPEC.md §6), parsed into plain structs.
// No LVGL or ESP-IDF in here, so it also builds for the host tests (test/host).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CFG_MAX_SCREENS   8
#define CFG_MAX_WIDGETS   24
#define CFG_MAX_ZONES     6
#define CFG_TEXT_LEN      48
#define CFG_PATH_LEN      96
#define CFG_UNIT_LEN      8
#define CFG_MAX_JSON      (32 * 1024)
#define CFG_STATE_LEN     24
#define CFG_MAX_STATES    8

#define CFG_DECIMALS_AUTO  -1   // from Signal K metadata (displayFormat), else 1

// Signal K zone states, mildest first.
typedef enum {
    ZONE_NONE, ZONE_NOMINAL, ZONE_NORMAL, ZONE_ALERT, ZONE_WARN, ZONE_ALARM, ZONE_EMERGENCY
} zone_state_t;

typedef struct {
    bool has_lower, has_upper;
    double lower, upper;   // SI, like the data
    zone_state_t state;
} zone_t;

typedef struct {
    char path[CFG_PATH_LEN];
    char unit[CFG_UNIT_LEN];   // "" = from Signal K metadata (displayUnits), else SI
    int decimals;              // CFG_DECIMALS_AUTO or 0..6
    int stale_after_s;
    int zone_count;            // > 0 overrides the zones in Signal K metadata
    zone_t zones[CFG_MAX_ZONES];
} binding_t;

// `state` widget: one value it can show, e.g. "bulk" -> "Bulk", amber.
typedef struct {
    char value[CFG_STATE_LEN]; // text as received; numbers as %g ("0", "1.5"); booleans "true"/"false"
    char text[CFG_STATE_LEN];  // shown; "" = the value itself
    zone_state_t state;        // colour, as for zones
} state_map_t;

typedef struct {
    int type;                  // index into the widget type table (widget_types.h)
    int col, row, w, h;
    char label[CFG_TEXT_LEN];  // "" = metadata displayName, else last path element
    binding_t value;           // used when the type takes a value binding
    double min, max;           // ring, bar: scale ends, SI like the data
    bool vertical;             // bar
    int state_count;           // state
    state_map_t states[CFG_MAX_STATES];
    char notify_prefix[CFG_PATH_LEN];   // alert: under notifications., "" = all
    char api[CFG_PATH_LEN];    // tide: tides API base, e.g. /signalk/v2/api/tides/stations/vessel/default;
                               // anchor: History API values endpoint, e.g. /signalk/v2/api/history/values ("" = none)
    int hours;                 // tide: hours ahead to show
    char pos_path[CFG_PATH_LEN];      // anchor: the boat's position
    char anchor_path[CFG_PATH_LEN];   // anchor: the anchor's position (null when raised)
    char radius_path[CFG_PATH_LEN];   // anchor: alarm circle radius, m
    char heading_path[CFG_PATH_LEN];  // anchor: heading for the boat mark, rad; "" = a dot
    int trail_min;                    // anchor: minutes of swing trail
} widget_cfg_t;

typedef struct {
    char id[CFG_TEXT_LEN];
    char title[CFG_TEXT_LEN];
    int cols, rows;
    int widget_count;
    widget_cfg_t widgets[CFG_MAX_WIDGETS];
} screen_cfg_t;

#define CFG_MAX_AUTO      12

// Switch screens from a Signal K text value, e.g. navigation.state "anchored" -> "anchor" screen.
typedef struct {
    char path[CFG_PATH_LEN];   // "" = off
    int manual_hold_s;         // after a swipe, wait this long before switching again
    int count;
    char states[CFG_MAX_AUTO][CFG_STATE_LEN];
    int screens[CFG_MAX_AUTO]; // index into config_t.screens
} auto_screen_t;

typedef struct {
    int schema_version;
    char device_name[CFG_TEXT_LEN];
    char hostname[32];
    char sk_host[64];          // "" = find the server by mDNS
    int sk_port;
    int screen_count;
    screen_cfg_t screens[CFG_MAX_SCREENS];
    auto_screen_t auto_screen;
    char clock_offset_path[CFG_PATH_LEN];   // Signal K (-)hhmm UTC offset, e.g. environment.time.timezoneOffset; "" = UTC
    bool clock_hour12;                      // "6:14 PM" instead of "18:14"
} config_t;

// Parses and validates JSON text into `cfg`. On failure returns false and writes a message to `err`.
bool config_parse(const char *json, config_t *cfg, char *err, int err_len);

// Zone state for `v` (SI): the most severe zone containing it (lower <= v <= upper), ZONE_NONE if none.
zone_state_t zones_state(const zone_t *zones, int count, double v);

// "alarm" -> ZONE_ALARM. False if the name is not a Signal K zone state.
bool zone_state_from_name(const char *name, zone_state_t *out);
const char *zone_state_name(zone_state_t s);
