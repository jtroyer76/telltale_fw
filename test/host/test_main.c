// Host tests for the parts with no ESP-IDF or LVGL in them: config parsing and validation,
// zones, units, Signal K timestamps. Build and run: test/host/run.sh
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "alerts.h"
#include "cJSON.h"
#include "config.h"
#include "sk_time.h"
#include "tides.h"
#include "history.h"
#include "track.h"
#include "units.h"
#include "widget_types.h"

static int s_run, s_failed;

#define CHECK(cond) do { \
    s_run++; \
    if (!(cond)) { s_failed++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define NEAR(a, b) (fabs((a) - (b)) < 1e-6)

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(n + 1);
    buf[fread(buf, 1, n, f)] = '\0';
    fclose(f);
    return buf;
}

static config_t s_cfg;
static char s_err[160];

static bool parse(const char *json) { return config_parse(json, &s_cfg, s_err, sizeof(s_err)); }

// A one-screen config with `widgets` (JSON array body) and optional extra top-level members.
static bool parse_screen(const char *widgets, const char *extra)
{
    char json[4096];
    snprintf(json, sizeof(json),
             "{\"schema_version\":1,\"screens\":[{\"id\":\"a\",\"grid\":{\"cols\":3,\"rows\":2},\"widgets\":[%s]},"
             "{\"id\":\"b\",\"widgets\":[]}]%s%s}",
             widgets, extra ? "," : "", extra ? extra : "");
    return parse(json);
}

#define VALUE(at, bind) "{\"type\":\"value\",\"at\":" at ",\"bind\":{\"value\":" bind "}}"

static void test_default_config(const char *path)
{
    char *json = read_file(path);
    CHECK(json != NULL);
    if (!json) return;
    CHECK(parse(json));
    // Generic: standard Signal K paths, the server's display units, found by mDNS
    CHECK(s_cfg.screen_count == 2);
    CHECK(strcmp(s_cfg.hostname, "telltale") == 0);
    CHECK(s_cfg.sk_host[0] == '\0');
    CHECK(s_cfg.screens[0].widget_count == 5);
    CHECK(s_cfg.screens[0].widgets[0].type == widget_type_find("value"));
    CHECK(strcmp(s_cfg.screens[0].widgets[0].value.path, "navigation.speedOverGround") == 0);
    CHECK(s_cfg.screens[0].widgets[0].value.unit[0] == '\0');
    CHECK(s_cfg.screens[0].widgets[0].value.decimals == CFG_DECIMALS_AUTO);
    CHECK(s_cfg.screens[0].widgets[0].value.stale_after_s == 60);
    CHECK(s_cfg.screens[1].widgets[6].type == widget_type_find("alert"));
    free(json);
}

// Every config under examples/ must pass the firmware's own checks.
static void test_example(const char *path)
{
    char *json = read_file(path);
    CHECK(json != NULL);
    bool ok = json && parse(json);
    if (!ok) printf("FAIL %s: %s\n", path, s_err);
    CHECK(ok);
    free(json);
}

static void test_defaults(void)
{
    CHECK(parse_screen(VALUE("{\"col\":0,\"row\":0}", "{\"path\":\"a.b.c\"}"), NULL));
    const widget_cfg_t *w = &s_cfg.screens[0].widgets[0];
    CHECK(w->w == 1 && w->h == 1);
    CHECK(w->label[0] == '\0');                       // filled from metadata at runtime
    CHECK(w->value.unit[0] == '\0');
    CHECK(w->value.decimals == CFG_DECIMALS_AUTO);
    CHECK(w->value.zone_count == 0);
    CHECK(strcmp(s_cfg.sk_host, "") == 0);           // mDNS
    CHECK(s_cfg.sk_port == 80);
    CHECK(strcmp(s_cfg.screens[1].title, "b") == 0); // title defaults to id
}

static void test_rejects(void)
{
    CHECK(!parse("not json"));
    CHECK(strstr(s_err, "not valid JSON") != NULL);
    CHECK(!parse("{\"schema_version\":2,\"screens\":[{\"id\":\"a\"}]}"));
    CHECK(!parse("{\"schema_version\":1,\"screens\":[]}"));
    CHECK(!parse("{\"schema_version\":1,\"screens\":[{\"title\":\"no id\"}]}"));
    CHECK(!parse("{\"schema_version\":1,\"screens\":[{\"id\":\"a\"},{\"id\":\"a\"}]}"));
    CHECK(!parse("{\"schema_version\":1,\"device\":{\"hostname\":\"Bad_Name\"},\"screens\":[{\"id\":\"a\"}]}"));
    CHECK(!parse("{\"schema_version\":1,\"signalk\":{\"port\":70000},\"screens\":[{\"id\":\"a\"}]}"));

    CHECK(!parse_screen("{\"type\":\"gauge\",\"at\":{\"col\":0,\"row\":0}}", NULL));
    CHECK(strstr(s_err, "unknown widget type") != NULL);
    CHECK(!parse_screen("{\"type\":\"value\",\"bind\":{\"value\":{\"path\":\"x\"}}}", NULL));     // no at
    CHECK(!parse_screen(VALUE("{\"col\":2,\"row\":0,\"w\":2}", "{\"path\":\"x\"}"), NULL));       // off grid
    CHECK(!parse_screen(VALUE("{\"col\":0,\"row\":0}", "{}"), NULL));                              // no path
    CHECK(!parse_screen(VALUE("{\"col\":0,\"row\":0}", "{\"path\":\"x\",\"unit\":\"furlong\"}"), NULL));
    CHECK(!parse_screen(VALUE("{\"col\":0,\"row\":0}", "{\"path\":\"x\",\"decimals\":9}"), NULL));
    CHECK(!parse_screen(VALUE("{\"col\":0,\"row\":0}", "{\"path\":\"x\",\"stale_after_s\":0}"), NULL));
    CHECK(!parse_screen(VALUE("{\"col\":0,\"row\":0}", "{\"path\":\"x\",\"zones\":[{\"state\":\"alarm\"}]}"), NULL));
    CHECK(!parse_screen(VALUE("{\"col\":0,\"row\":0}", "{\"path\":\"x\",\"zones\":[{\"upper\":1,\"state\":\"panic\"}]}"), NULL));
    CHECK(!parse_screen("{\"type\":\"label\",\"at\":{\"col\":0,\"row\":0}}", NULL));               // label needs text

    // Overlap
    CHECK(!parse_screen(VALUE("{\"col\":0,\"row\":0,\"w\":2}", "{\"path\":\"x\"}") ","
                        VALUE("{\"col\":1,\"row\":0}", "{\"path\":\"y\"}"), NULL));
    CHECK(strstr(s_err, "overlap") != NULL);
    CHECK(parse_screen(VALUE("{\"col\":0,\"row\":0,\"w\":2}", "{\"path\":\"x\"}") ","
                       VALUE("{\"col\":2,\"row\":0}", "{\"path\":\"y\"}"), NULL));

    // Too long to store: rejected, never cut short
    char path[200];
    memset(path, 'p', sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    char w[400];
    snprintf(w, sizeof(w), "{\"type\":\"value\",\"at\":{\"col\":0,\"row\":0},\"bind\":{\"value\":{\"path\":\"%s\"}}}", path);
    CHECK(!parse_screen(w, NULL));
    CHECK(strstr(s_err, "longer than") != NULL);
}

static void test_zones(void)
{
    CHECK(parse_screen(VALUE("{\"col\":0,\"row\":0}",
                             "{\"path\":\"x\",\"zones\":[{\"upper\":0.5,\"state\":\"alarm\"},"
                             "{\"lower\":0.5,\"upper\":0.7,\"state\":\"warn\"}]}"), NULL));
    const binding_t *b = &s_cfg.screens[0].widgets[0].value;
    CHECK(b->zone_count == 2);
    CHECK(zones_state(b->zones, b->zone_count, 0.2) == ZONE_ALARM);
    CHECK(zones_state(b->zones, b->zone_count, 0.6) == ZONE_WARN);
    CHECK(zones_state(b->zones, b->zone_count, 0.5) == ZONE_ALARM);   // both contain it: worst wins
    CHECK(zones_state(b->zones, b->zone_count, 0.9) == ZONE_NONE);

    zone_state_t z;
    CHECK(zone_state_from_name("emergency", &z) && z == ZONE_EMERGENCY);
    CHECK(!zone_state_from_name("none", &z));
    CHECK(strcmp(zone_state_name(ZONE_WARN), "warn") == 0);
}

static void test_auto_screen(void)
{
    CHECK(parse_screen("", "\"auto_screen\":{\"path\":\"navigation.state\",\"screens\":{\"anchored\":\"b\",\"sailing\":\"a\"}}"));
    const auto_screen_t *a = &s_cfg.auto_screen;
    CHECK(strcmp(a->path, "navigation.state") == 0);
    CHECK(a->manual_hold_s == 300);
    CHECK(a->count == 2);
    CHECK(strcmp(a->states[0], "anchored") == 0 && a->screens[0] == 1);
    CHECK(strcmp(a->states[1], "sailing") == 0 && a->screens[1] == 0);

    CHECK(parse_screen("", NULL));
    CHECK(s_cfg.auto_screen.path[0] == '\0');
    CHECK(!parse_screen("", "\"auto_screen\":{\"path\":\"navigation.state\",\"screens\":{\"anchored\":\"nope\"}}"));
    CHECK(!parse_screen("", "\"auto_screen\":{\"screens\":{}}"));
}

static void test_units(void)
{
    double v;
    CHECK(units_convert("degF", 273.15, &v) && NEAR(v, 32.0));
    CHECK(units_convert("degC", 300.0, &v) && NEAR(v, 26.85));
    CHECK(units_convert("kn", 1852.0 / 3600.0, &v) && NEAR(v, 1.0));
    CHECK(units_convert("%", 0.42, &v) && NEAR(v, 42.0));
    CHECK(units_convert("h", 7200, &v) && NEAR(v, 2.0));
    CHECK(units_convert("ft", 0.3048, &v) && NEAR(v, 1.0));
    CHECK(!units_convert("furlong", 1.0, &v) && v == 1.0);
    CHECK(units_known("") && units_known("Wh") && !units_known("btu"));

    // displayUnits as a real server sends them (2026-10-28)
    CHECK(strcmp(units_from_sk("K", "F"), "degF") == 0);
    CHECK(strcmp(units_from_sk("m", "foot"), "ft") == 0);
    CHECK(strcmp(units_from_sk("m/s", "kn"), "kn") == 0);
    CHECK(strcmp(units_from_sk("ratio", "percent"), "%") == 0);
    CHECK(strcmp(units_from_sk("s", "hour"), "h") == 0);
    CHECK(strcmp(units_from_sk("rad", "degree"), "deg") == 0);
    CHECK(strcmp(units_from_sk("V", "V"), "V") == 0);
    CHECK(units_from_sk("J", "btu") == NULL);     // not in the table: shown in SI
    CHECK(units_from_sk("m", "F") == NULL);       // doesn't convert from metres
    CHECK(units_from_sk(NULL, "F") == NULL);

    CHECK(units_decimals_from_format("0.00") == 2);
    CHECK(units_decimals_from_format("0.0") == 1);
    CHECK(units_decimals_from_format("0") == 0);
    CHECK(units_decimals_from_format("#") == -1);
}

static void test_sk_time(void)
{
    int64_t ms;
    CHECK(sk_time_parse("1970-01-01T00:00:00Z", &ms) && ms == 0);
    CHECK(sk_time_parse("2026-10-28T03:28:56.626Z", &ms) && ms == 1793158136626LL);
    CHECK(sk_time_parse("2026-10-28T03:28:56.339343Z", &ms) && ms == 1793158136339LL);   // microseconds
    CHECK(sk_time_parse("2026-10-28T03:28:56.5Z", &ms) && ms == 1793158136500LL);
    CHECK(sk_time_parse("2026-10-28T03:28:56Z", &ms) && ms == 1793158136000LL);
    CHECK(sk_time_parse("2024-02-29T12:00:00+00:00", &ms) && ms == 1709208000000LL);    // leap day
    CHECK(!sk_time_parse("2026-10-28T03:28:56", &ms));        // no zone
    CHECK(!sk_time_parse("2026-10-28T03:28:56+02:00", &ms));  // not UTC
    CHECK(!sk_time_parse("2026-13-28T03:28:56Z", &ms));
    CHECK(!sk_time_parse("garbage", &ms));
    CHECK(!sk_time_parse(NULL, &ms));
}

#define W(type, extra) "{\"type\":\"" type "\",\"at\":{\"col\":0,\"row\":0}" extra ",\"bind\":{\"value\":{\"path\":\"x\"}}}"

static void test_gauges_and_states(void)
{
    // ring / bar: max required, min defaults to 0, min < max
    CHECK(parse_screen(W("ring", ",\"max\":1"), NULL));
    widget_cfg_t *w = &s_cfg.screens[0].widgets[0];
    CHECK(w->type == widget_type_find("ring") && NEAR(w->min, 0) && NEAR(w->max, 1));
    CHECK(!parse_screen(W("ring", ""), NULL));
    CHECK(strstr(s_err, "max") != NULL);
    CHECK(!parse_screen(W("ring", ",\"min\":5,\"max\":5"), NULL));
    CHECK(!parse_screen(W("bar", ",\"max\":\"10\""), NULL));
    CHECK(!parse_screen("{\"type\":\"ring\",\"at\":{\"col\":0,\"row\":0},\"max\":1}", NULL));   // no binding

    // bar orientation: from the cell shape unless given
    CHECK(parse_screen("{\"type\":\"bar\",\"at\":{\"col\":0,\"row\":0,\"h\":2},\"min\":11,\"max\":15,"
                       "\"bind\":{\"value\":{\"path\":\"x\"}}}", NULL));
    CHECK(s_cfg.screens[0].widgets[0].vertical && NEAR(s_cfg.screens[0].widgets[0].min, 11));
    CHECK(parse_screen(W("bar", ",\"max\":1"), NULL) && !s_cfg.screens[0].widgets[0].vertical);
    CHECK(parse_screen(W("bar", ",\"max\":1,\"orientation\":\"vertical\""), NULL) &&
          s_cfg.screens[0].widgets[0].vertical);
    CHECK(!parse_screen(W("bar", ",\"max\":1,\"orientation\":\"diagonal\""), NULL));

    // state: map optional; value required and unique; text defaults to ""; state defaults to normal
    CHECK(parse_screen(W("state", ""), NULL) && s_cfg.screens[0].widgets[0].state_count == 0);
    CHECK(parse_screen(W("state", ",\"states\":[{\"value\":\"bulk\",\"text\":\"Bulk\",\"state\":\"alert\"},"
                                  "{\"value\":\"float\"}]"), NULL));
    w = &s_cfg.screens[0].widgets[0];
    CHECK(w->state_count == 2 && strcmp(w->states[0].text, "Bulk") == 0 && w->states[0].state == ZONE_ALERT);
    CHECK(w->states[1].text[0] == '\0' && w->states[1].state == ZONE_NORMAL);
    CHECK(!parse_screen(W("state", ",\"states\":[{\"text\":\"no value\"}]"), NULL));
    CHECK(!parse_screen(W("state", ",\"states\":[{\"value\":\"a\"},{\"value\":\"a\"}]"), NULL));
    CHECK(strstr(s_err, "twice") != NULL);
    CHECK(!parse_screen(W("state", ",\"states\":[{\"value\":\"a\",\"state\":\"panic\"}]"), NULL));
    CHECK(!parse_screen(W("state", ",\"states\":[{\"value\":1}]"), NULL));   // values are written as text
    CHECK(!parse_screen(W("state", ",\"states\":[{\"value\":\"a\"},{\"value\":\"b\"},{\"value\":\"c\"},"
                                  "{\"value\":\"d\"},{\"value\":\"e\"},{\"value\":\"f\"},{\"value\":\"g\"},"
                                  "{\"value\":\"h\"},{\"value\":\"i\"}]"), NULL));
}

static void count_pattern(const char *path, void *ctx)
{
    char *buf = ctx;
    strcat(buf, path);
    strcat(buf, ";");
}

static void test_alerts(void)
{
    // config: prefix optional, either form, no wildcards; label optional
    CHECK(parse_screen("{\"type\":\"alert\",\"at\":{\"col\":0,\"row\":0}}", NULL));
    CHECK(s_cfg.screens[0].widgets[0].notify_prefix[0] == '\0');
    CHECK(parse_screen("{\"type\":\"alert\",\"at\":{\"col\":0,\"row\":0},"
                       "\"bind\":{\"notifications\":{\"prefix\":\"notifications.electrical.batteries\"}}}", NULL));
    CHECK(strcmp(s_cfg.screens[0].widgets[0].notify_prefix, "electrical.batteries") == 0);
    CHECK(parse_screen("{\"type\":\"alert\",\"at\":{\"col\":0,\"row\":0},"
                       "\"bind\":{\"notifications\":{\"prefix\":\"navigation.anchor\"}}}", NULL));
    CHECK(strcmp(s_cfg.screens[0].widgets[0].notify_prefix, "navigation.anchor") == 0);
    CHECK(!parse_screen("{\"type\":\"alert\",\"at\":{\"col\":0,\"row\":0},"
                        "\"bind\":{\"notifications\":{\"prefix\":\"electrical.*\"}}}", NULL));
    CHECK(!parse_screen("{\"type\":\"alert\",\"at\":{\"col\":0,\"row\":0},"
                        "\"bind\":{\"notifications\":{\"prefix\":5}}}", NULL));

    // prefix matching: whole path elements only
    CHECK(alerts_prefix_match("", "anything.at.all"));
    CHECK(alerts_prefix_match("electrical", "electrical.batteries.shunt"));
    CHECK(alerts_prefix_match("electrical.batteries", "electrical.batteries"));
    CHECK(!alerts_prefix_match("electrical.bat", "electrical.batteries"));
    CHECK(!alerts_prefix_match("navigation", "electrical.batteries"));

    // the table
    alerts_init();
    alerts_reset();
    alerts_set_live(true);
    alert_t a;
    CHECK(alerts_worst("", &a) == 0);
    alerts_set("navigation.anchor", "normal", "Started");          // normal: not active
    alerts_set("server.newVersion", "nominal", "A new version");
    CHECK(alerts_worst("", &a) == 0);
    alerts_set("electrical.batteries.shunt.voltage", "warn", "Battery low");
    alerts_set("navigation.anchor", "alarm", "Anchor dragging");
    alerts_set("environment.bilge", "alert", "Bilge ran");
    CHECK(alerts_worst("", &a) == 3);
    CHECK(strcmp(a.path, "navigation.anchor") == 0 && a.state == ZONE_ALARM);   // most severe
    CHECK(alerts_worst("electrical", &a) == 1 && strcmp(a.message, "Battery low") == 0);
    alerts_set("environment.fridge", "alarm", "Fridge warm");                    // same severity, newer
    CHECK(alerts_worst("", &a) == 4 && strcmp(a.path, "environment.fridge") == 0);
    alerts_set("environment.fridge", "normal", "");                              // back to normal: gone
    alerts_set("navigation.anchor", NULL, NULL);                                 // null value: gone
    CHECK(alerts_worst("", &a) == 2 && strcmp(a.path, "electrical.batteries.shunt.voltage") == 0);
    alerts_set("x.y", "weird", "?");                                             // unknown state shows as alert
    CHECK(alerts_worst("x", &a) == 1 && a.state == ZONE_ALERT);

    // going live again starts empty (the server resends); not live is reported
    alerts_set_live(false);
    CHECK(!alerts_live());
    alerts_set_live(true);
    CHECK(alerts_live() && alerts_worst("", &a) == 0);

    // subscription patterns for the screen on show only
    alerts_reset();
    CHECK(alerts_watch("electrical.batteries", 0));
    CHECK(alerts_watch("", 1));
    char buf[256] = "";
    CHECK(alerts_for_each_subscribed(0, count_pattern, buf) == 2);
    CHECK(strcmp(buf, "notifications.electrical.batteries;notifications.electrical.batteries.*;") == 0);
    buf[0] = '\0';
    CHECK(alerts_for_each_subscribed(1, count_pattern, buf) == 1 && strcmp(buf, "notifications.*;") == 0);
    buf[0] = '\0';
    CHECK(alerts_for_each_subscribed(2, count_pattern, buf) == 0);
}

static void test_clock(void)
{
    char buf[32];
    int64_t ms;
    CHECK(sk_time_parse("2026-10-28T20:42:55.011Z", &ms));
    sk_time_format_iso(ms, buf, sizeof(buf));
    CHECK(strcmp(buf, "2026-10-28T20:42:55Z") == 0);
    CHECK(sk_time_parse("2024-02-29T23:59:59Z", &ms));   // leap day round trip
    sk_time_format_iso(ms, buf, sizeof(buf));
    CHECK(strcmp(buf, "2024-02-29T23:59:59Z") == 0);
    sk_time_format_iso(0, buf, sizeof(buf));
    CHECK(strcmp(buf, "1970-01-01T00:00:00Z") == 0);

    // local clock text: PDT is UTC-7
    CHECK(sk_time_parse("2026-10-28T20:42:55.011Z", &ms));
    sk_time_format_clock(ms, -7 * 60, true, buf, sizeof(buf));
    CHECK(strcmp(buf, "1:42 PM") == 0);
    sk_time_format_clock(ms, -7 * 60, false, buf, sizeof(buf));
    CHECK(strcmp(buf, "13:42") == 0);
    CHECK(sk_time_parse("2026-10-29T08:46:50Z", &ms));
    sk_time_format_clock(ms, -7 * 60, true, buf, sizeof(buf));
    CHECK(strcmp(buf, "1:46 AM") == 0);
    CHECK(sk_time_parse("2026-10-29T07:05:00Z", &ms));   // local midnight hour
    sk_time_format_clock(ms, -7 * 60, true, buf, sizeof(buf));
    CHECK(strcmp(buf, "12:05 AM") == 0);
    CHECK(sk_time_parse("2026-10-29T19:30:00Z", &ms));   // noon hour
    sk_time_format_clock(ms, -7 * 60, true, buf, sizeof(buf));
    CHECK(strcmp(buf, "12:30 PM") == 0);
    sk_time_format_clock(ms, 0, false, buf, sizeof(buf));
    CHECK(strcmp(buf, "19:30") == 0);
    sk_time_format_clock(ms, 5 * 60 + 30, false, buf, sizeof(buf));   // +0530 crosses midnight
    CHECK(strcmp(buf, "01:00") == 0);

    // Signal K timezoneOffset (-)hhmm
    int m;
    CHECK(sk_time_offset_minutes(-700, &m) && m == -420);
    CHECK(sk_time_offset_minutes(530, &m) && m == 330);
    CHECK(sk_time_offset_minutes(-930, &m) && m == -570);
    CHECK(sk_time_offset_minutes(0, &m) && m == 0);
    CHECK(!sk_time_offset_minutes(-775, &m));    // 75 minutes
    CHECK(!sk_time_offset_minutes(1600, &m));
    CHECK(!sk_time_offset_minutes(-7.5, &m));    // hours, not hhmm
}

static tides_t s_tides;

// Real signalk-tides responses (a NOAA station, renamed), saved 2026-10-28.
static void test_tides(void)
{
    char *ext = read_file("test/host/fixtures/tides_extremes.json");
    char *tl = read_file("test/host/fixtures/tides_timeline.json");
    CHECK(ext && tl);
    if (!ext || !tl) return;
    memset(&s_tides, 0, sizeof(s_tides));
    CHECK(tides_parse_timeline(tl, &s_tides));
    CHECK(tides_parse_extremes(ext, &s_tides));
    CHECK(strcmp(s_tides.station, "Sample Station") == 0);
    CHECK(strcmp(s_tides.datum, "MLLW") == 0);
    CHECK(s_tides.n_pts == 121 && s_tides.n_ext == 3);
    CHECK(!s_tides.ext[0].high && s_tides.ext[1].high && !s_tides.ext[2].high);
    CHECK(fabs(s_tides.ext[2].level + 0.6889) < 0.001);   // the overnight low, -0.69 m

    int64_t t0, low_ms, t;
    CHECK(sk_time_parse("2026-10-28T18:00:00Z", &t0));
    float level;
    CHECK(tides_level_at(&s_tides, t0, &level) && fabs(level - 2.5967) < 0.001);
    CHECK(!tides_level_at(&s_tides, t0 - 60000, &level));            // before the timeline
    CHECK(sk_time_parse("2026-10-28T18:05:00Z", &t));                 // between two points
    CHECK(tides_level_at(&s_tides, t, &level) && level < 2.5967f && level > 2.4f);

    // lowest low in a window: the overnight one, not the afternoon one
    CHECK(sk_time_parse("2026-10-29T08:46:50.305Z", &low_ms));
    int i = tides_lowest(&s_tides, t0, t0 + 18 * 3600000LL);
    CHECK(i == 2 && s_tides.ext[i].ms == low_ms);
    CHECK(tides_lowest(&s_tides, t0, t0 + 6 * 3600000LL) == 0);       // only the afternoon low in 6 h
    CHECK(tides_lowest(&s_tides, low_ms + 1, low_ms + 3600000LL) == -1);

    // wrong shapes are refused
    tides_t bad;
    memset(&bad, 0, sizeof(bad));
    CHECK(!tides_parse_extremes("{\"units\":\"feet\",\"extremes\":[]}", &bad));
    CHECK(!tides_parse_timeline("{\"timeline\":[{\"time\":\"2026-10-28T18:00:00Z\",\"level\":1}]}", &bad));   // one point
    CHECK(!tides_parse_timeline("not json", &bad));
    free(ext);
    free(tl);

    // config: tide widget and the clock
    CHECK(parse_screen("{\"type\":\"tide\",\"at\":{\"col\":0,\"row\":0,\"w\":3,\"h\":2},"
                       "\"api\":\"/signalk/v2/api/tides/stations/vessel/default\","
                       "\"bind\":{\"value\":{\"path\":\"environment.tide.heightNow\",\"unit\":\"ft\"}}}",
                       "\"clock\":{\"offset_path\":\"environment.time.timezoneOffset\",\"hour12\":true}"));
    widget_cfg_t *w = &s_cfg.screens[0].widgets[0];
    CHECK(w->hours == 18 && strcmp(w->api, "/signalk/v2/api/tides/stations/vessel/default") == 0);
    CHECK(strcmp(s_cfg.clock_offset_path, "environment.time.timezoneOffset") == 0 && s_cfg.clock_hour12);
    CHECK(parse_screen(VALUE("{\"col\":0,\"row\":0}", "{\"path\":\"x\"}"), NULL) &&
          s_cfg.clock_offset_path[0] == '\0' && !s_cfg.clock_hour12);           // no clock: UTC, 24 h
    CHECK(!parse_screen("{\"type\":\"tide\",\"at\":{\"col\":0,\"row\":0},"
                        "\"bind\":{\"value\":{\"path\":\"environment.tide.heightNow\"}}}", NULL));   // no api
    CHECK(!parse_screen("{\"type\":\"tide\",\"at\":{\"col\":0,\"row\":0},\"api\":\"/x\",\"hours\":72,"
                        "\"bind\":{\"value\":{\"path\":\"environment.tide.heightNow\"}}}", NULL));
    CHECK(!parse_screen(VALUE("{\"col\":0,\"row\":0}", "{\"path\":\"x\"}"), "\"clock\":{\"hour12\":\"yes\"}"));
}

static track_t s_track;

static void test_anchor(void)
{
    // geometry: 0.001 deg of latitude ~ 111 m north; longitude shrinks with cos(lat)
    double e, n;
    geo_offset_m(45.0, -123.0, 45.001, -123.0, &e, &n);
    CHECK(fabs(n - 111.32) < 0.1 && fabs(e) < 1e-9);
    geo_offset_m(45.0, -123.0, 45.0, -122.999, &e, &n);
    CHECK(fabs(e - 78.7) < 0.5 && fabs(n) < 1e-9);   // 111320 * cos(45 deg) * 0.001

    // trail: at most one fix per 5 s, only if it moved 0.5 m, but at least one a minute
    track_reset(&s_track);
    CHECK(track_add(&s_track, 45.0, -123.0, 0));
    CHECK(!track_add(&s_track, 45.0001, -123.0, 4000));        // too soon
    CHECK(track_add(&s_track, 45.0001, -123.0, 5000));         // 11 m, 5 s later
    CHECK(!track_add(&s_track, 45.0001, -123.0, 30000));       // hasn't moved
    CHECK(track_add(&s_track, 45.0001, -123.0, 65000));        // a minute: kept anyway
    CHECK(s_track.count == 3 && track_at(&s_track, 0)->ms == 0 && track_at(&s_track, 2)->ms == 65000);
    CHECK(track_at(&s_track, 1)->lat_e7 == 450001000);
    // the ring keeps the newest TRACK_MAX
    track_reset(&s_track);
    for (int i = 0; i < TRACK_MAX + 10; i++) track_add(&s_track, 48.0 + i * 1e-4, -122.0, (int64_t)i * 60000);
    CHECK(s_track.count == TRACK_MAX && track_at(&s_track, 0)->ms == 10 * 60000LL);
    CHECK(track_at(&s_track, TRACK_MAX - 1)->ms == (int64_t)(TRACK_MAX + 9) * 60000);

    // config
    const char *ok_anchor = "{\"type\":\"anchor\",\"at\":{\"col\":0,\"row\":0,\"w\":2,\"h\":2},"
        "\"bind\":{\"value\":{\"path\":\"navigation.anchor.currentRadius\",\"unit\":\"ft\"},"
        "\"position\":{\"path\":\"navigation.position\"},\"anchor\":{\"path\":\"navigation.anchor.position\"},"
        "\"radius\":{\"path\":\"navigation.anchor.maxRadius\"},\"notifications\":{\"prefix\":\"navigation.anchor\"}}}";
    CHECK(parse_screen(ok_anchor, NULL));
    widget_cfg_t *w = &s_cfg.screens[0].widgets[0];
    CHECK(strcmp(w->anchor_path, "navigation.anchor.position") == 0 && strcmp(w->pos_path, "navigation.position") == 0);
    CHECK(w->heading_path[0] == '\0' && w->trail_min == 240 && strcmp(w->notify_prefix, "navigation.anchor") == 0);
    CHECK(!parse_screen("{\"type\":\"anchor\",\"at\":{\"col\":0,\"row\":0},"
                        "\"bind\":{\"value\":{\"path\":\"a\"},\"position\":{\"path\":\"b\"},\"anchor\":{\"path\":\"c\"}}}", NULL));
    CHECK(strstr(s_err, "radius") != NULL);
    CHECK(!parse_screen("{\"type\":\"anchor\",\"at\":{\"col\":0,\"row\":0},\"trail_minutes\":5,"
                        "\"bind\":{\"value\":{\"path\":\"a\"},\"position\":{\"path\":\"b\"},\"anchor\":{\"path\":\"c\"},"
                        "\"radius\":{\"path\":\"d\"}}}", NULL));
}

static track_pt_t s_hist[TRACK_MAX];
static track_t s_scratch;

static void test_history(void)
{
    // A real History API response (2026-10-28 12:00-13:00 UTC, 30 s rows, positions moved)
    char *json = read_file("test/host/fixtures/history_positions.json");
    CHECK(json != NULL);
    if (!json) return;
    int n = history_parse_positions(json, s_hist, TRACK_MAX);
    CHECK(n == 120);
    int64_t ms;
    CHECK(sk_time_parse("2026-10-28T12:00:00.000Z", &ms) && s_hist[0].ms == ms);   // not the "range" times
    CHECK(s_hist[0].lat_e7 == 450000082 && s_hist[0].lon_e7 == -1229999749);         // [lon, lat] read right
    CHECK(s_hist[119].ms - s_hist[0].ms == 119 * 30000LL);
    CHECK(history_parse_positions(json, s_hist, 10) == 10);                           // stops at max
    free(json);

    CHECK(history_parse_positions("{\"data\":[[\"2026-10-28T12:00:00Z\",null],"
                                  "[\"2026-10-28T12:00:30Z\",[-122.5,48.5]]]}", s_hist, 10) == 1);   // null row skipped
    CHECK(history_parse_positions("{\"data\":[]}", s_hist, 10) == 0);
    CHECK(history_parse_positions("{\"error\":\"no provider\"}", s_hist, 10) == 0);
    CHECK(history_parse_positions(NULL, s_hist, 10) == 0);

    // backfill: history goes in front of the live points; overlap with them is dropped
    track_reset(&s_track);
    track_add(&s_track, 48.0, -122.0, 1000000);
    track_add(&s_track, 48.001, -122.0, 1010000);
    for (int i = 0; i < 5; i++) {
        s_hist[i] = (track_pt_t){ .lat_e7 = 480000000 - i, .lon_e7 = -1220000000, .ms = 970000 + i * 10000 };
    }   // 970, 980, 990, 1000 (overlaps the first live point), 1010
    track_backfill(&s_track, s_hist, 5, &s_scratch);
    CHECK(s_track.count == 5);
    CHECK(track_at(&s_track, 0)->ms == 970000 && track_at(&s_track, 2)->ms == 990000);
    CHECK(track_at(&s_track, 3)->ms == 1000000 && track_at(&s_track, 3)->lat_e7 == 480000000);   // the live one
    CHECK(track_add(&s_track, 48.002, -122.0, 1020000) && track_at(&s_track, 5)->ms == 1020000);   // live goes on
    // too many: the oldest go
    track_reset(&s_track);
    track_add(&s_track, 48.0, -122.0, (int64_t)TRACK_MAX * 1000);
    for (int i = 0; i < TRACK_MAX; i++) s_hist[i] = (track_pt_t){ .lat_e7 = 480000000, .lon_e7 = -1220000000, .ms = i };
    track_backfill(&s_track, s_hist, TRACK_MAX, &s_scratch);
    CHECK(s_track.count == TRACK_MAX && track_at(&s_track, 0)->ms == 1 &&
          track_at(&s_track, TRACK_MAX - 1)->ms == (int64_t)TRACK_MAX * 1000);

    // config
    CHECK(parse_screen("{\"type\":\"anchor\",\"at\":{\"col\":0,\"row\":0},\"history\":\"/signalk/v2/api/history/values\","
                       "\"bind\":{\"value\":{\"path\":\"a\"},\"position\":{\"path\":\"b\"},\"anchor\":{\"path\":\"c\"},"
                       "\"radius\":{\"path\":\"d\"}}}", NULL));
    CHECK(strcmp(s_cfg.screens[0].widgets[0].api, "/signalk/v2/api/history/values") == 0);
    CHECK(!parse_screen("{\"type\":\"anchor\",\"at\":{\"col\":0,\"row\":0},\"history\":\"http://x\","
                        "\"bind\":{\"value\":{\"path\":\"a\"},\"position\":{\"path\":\"b\"},\"anchor\":{\"path\":\"c\"},"
                        "\"radius\":{\"path\":\"d\"}}}", NULL));
}

static void test_widget_types(void)
{
    CHECK(widget_type_find("value") >= 0);
    CHECK(widget_type_find("label") >= 0);
    CHECK(widget_type_find("ring") >= 0 && widget_type_find("bar") >= 0 && widget_type_find("state") >= 0);
    CHECK(widget_type_find("alert") >= 0 && widget_type_find("tide") >= 0 && widget_type_find("anchor") >= 0);
    CHECK(widget_type_find("gauge") == -1);
    for (int i = 0; i < widget_type_count; i++) {
        cJSON *d = cJSON_Parse(widget_types[i].descriptor);   // served as-is at /api/widgets
        CHECK(d != NULL);
        CHECK(d && strcmp(cJSON_GetObjectItem(d, "type")->valuestring, widget_types[i].name) == 0);
        cJSON_Delete(d);
    }
}

int main(int argc, char **argv)
{
    test_default_config(argc > 1 ? argv[1] : "data/default_config.json");
    for (int i = 2; i < argc; i++) test_example(argv[i]);
    test_defaults();
    test_rejects();
    test_zones();
    test_auto_screen();
    test_units();
    test_sk_time();
    test_gauges_and_states();
    test_alerts();
    test_clock();
    test_tides();
    test_anchor();
    test_history();
    test_widget_types();
    printf("%d checks, %d failed\n", s_run, s_failed);
    return s_failed ? 1 : 0;
}
