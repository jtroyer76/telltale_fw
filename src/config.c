#include "config.h"

#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "units.h"
#include "widget_types.h"

#define SCHEMA_VERSION         1
#define DEFAULT_STALE_AFTER_S  60
#define MAX_DECIMALS           6

static const char *s_zone_names[] = {
    [ZONE_NONE] = "none", [ZONE_NOMINAL] = "nominal", [ZONE_NORMAL] = "normal", [ZONE_ALERT] = "alert",
    [ZONE_WARN] = "warn", [ZONE_ALARM] = "alarm", [ZONE_EMERGENCY] = "emergency",
};

bool zone_state_from_name(const char *name, zone_state_t *out)
{
    for (int i = ZONE_NOMINAL; i <= ZONE_EMERGENCY; i++) {
        if (strcmp(s_zone_names[i], name) == 0) {
            *out = (zone_state_t)i;
            return true;
        }
    }
    return false;
}

const char *zone_state_name(zone_state_t s) { return s_zone_names[s]; }

zone_state_t zones_state(const zone_t *zones, int count, double v)
{
    zone_state_t worst = ZONE_NONE;
    for (int i = 0; i < count; i++) {
        const zone_t *z = &zones[i];
        if ((z->has_lower && v < z->lower) || (z->has_upper && v > z->upper)) continue;
        if (z->state > worst) worst = z->state;
    }
    return worst;
}

static void copy_str(char *dst, int len, const cJSON *item, const char *fallback)
{
    const char *s = cJSON_IsString(item) ? item->valuestring : fallback;
    snprintf(dst, len, "%s", s ? s : "");
}

static int get_int(const cJSON *obj, const char *key, int fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? v->valueint : fallback;
}

// Strings must fit their field: silently cutting a path would bind the wrong value.
static bool check_len(const cJSON *item, int max, const char *what, const char *where, char *err, int err_len)
{
    if (cJSON_IsString(item) && (int)strlen(item->valuestring) >= max) {
        snprintf(err, err_len, "%s: %s is longer than %d characters", where, what, max - 1);
        return false;
    }
    return true;
}

static bool parse_zones(const cJSON *jz, binding_t *b, const char *where, char *err, int err_len)
{
    b->zone_count = 0;
    if (!jz) return true;
    if (!cJSON_IsArray(jz)) {
        snprintf(err, err_len, "%s: zones must be an array", where);
        return false;
    }
    const cJSON *z;
    cJSON_ArrayForEach(z, jz) {
        if (b->zone_count >= CFG_MAX_ZONES) {
            snprintf(err, err_len, "%s: more than %d zones", where, CFG_MAX_ZONES);
            return false;
        }
        zone_t *out = &b->zones[b->zone_count];
        const cJSON *lower = cJSON_GetObjectItemCaseSensitive(z, "lower");
        const cJSON *upper = cJSON_GetObjectItemCaseSensitive(z, "upper");
        const cJSON *state = cJSON_GetObjectItemCaseSensitive(z, "state");
        out->has_lower = cJSON_IsNumber(lower);
        out->has_upper = cJSON_IsNumber(upper);
        out->lower = out->has_lower ? lower->valuedouble : 0;
        out->upper = out->has_upper ? upper->valuedouble : 0;
        if (!out->has_lower && !out->has_upper) {
            snprintf(err, err_len, "%s: zone %d has neither lower nor upper", where, b->zone_count);
            return false;
        }
        if (!cJSON_IsString(state) || !zone_state_from_name(state->valuestring, &out->state)) {
            snprintf(err, err_len, "%s: zone %d state must be nominal, normal, alert, warn, alarm or emergency",
                     where, b->zone_count);
            return false;
        }
        b->zone_count++;
    }
    return true;
}

static bool parse_value_binding(const cJSON *value, binding_t *b, const char *where, char *err, int err_len)
{
    const cJSON *path = cJSON_GetObjectItemCaseSensitive(value, "path");
    if (!cJSON_IsString(path) || path->valuestring[0] == '\0') {
        snprintf(err, err_len, "%s: no bind.value.path", where);
        return false;
    }
    if (!check_len(path, CFG_PATH_LEN, "path", where, err, err_len)) return false;
    copy_str(b->path, sizeof(b->path), path, "");

    const cJSON *unit = cJSON_GetObjectItemCaseSensitive(value, "unit");
    if (unit && (!cJSON_IsString(unit) || !units_known(unit->valuestring))) {
        snprintf(err, err_len, "%s: unknown unit '%s'", where, cJSON_IsString(unit) ? unit->valuestring : "?");
        return false;
    }
    copy_str(b->unit, sizeof(b->unit), unit, "");

    b->decimals = get_int(value, "decimals", CFG_DECIMALS_AUTO);
    if (b->decimals != CFG_DECIMALS_AUTO && (b->decimals < 0 || b->decimals > MAX_DECIMALS)) {
        snprintf(err, err_len, "%s: decimals must be 0-%d", where, MAX_DECIMALS);
        return false;
    }
    b->stale_after_s = get_int(value, "stale_after_s", DEFAULT_STALE_AFTER_S);
    if (b->stale_after_s < 1) {
        snprintf(err, err_len, "%s: stale_after_s must be at least 1", where);
        return false;
    }
    return parse_zones(cJSON_GetObjectItemCaseSensitive(value, "zones"), b, where, err, err_len);
}

// ring, bar: max is required so a gauge can't silently peg on a default that doesn't suit it.
static bool parse_range(const cJSON *jw, widget_cfg_t *w, const char *where, char *err, int err_len)
{
    const cJSON *min = cJSON_GetObjectItemCaseSensitive(jw, "min");
    const cJSON *max = cJSON_GetObjectItemCaseSensitive(jw, "max");
    if ((min && !cJSON_IsNumber(min)) || !cJSON_IsNumber(max)) {
        snprintf(err, err_len, "%s: needs a numeric max (and min, default 0), in SI", where);
        return false;
    }
    w->min = min ? min->valuedouble : 0;
    w->max = max->valuedouble;
    if (w->min >= w->max) {
        snprintf(err, err_len, "%s: min must be less than max", where);
        return false;
    }
    return true;
}

static bool parse_orientation(const cJSON *jw, widget_cfg_t *w, const char *where, char *err, int err_len)
{
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(jw, "orientation");
    if (!o) {
        w->vertical = w->h > w->w;
    } else if (cJSON_IsString(o) && strcmp(o->valuestring, "vertical") == 0) {
        w->vertical = true;
    } else if (cJSON_IsString(o) && strcmp(o->valuestring, "horizontal") == 0) {
        w->vertical = false;
    } else {
        snprintf(err, err_len, "%s: orientation must be horizontal or vertical", where);
        return false;
    }
    return true;
}

static bool parse_states(const cJSON *jw, widget_cfg_t *w, const char *where, char *err, int err_len)
{
    const cJSON *js = cJSON_GetObjectItemCaseSensitive(jw, "states");
    w->state_count = 0;
    if (!js) return true;   // no map: every value shown as received
    if (!cJSON_IsArray(js)) {
        snprintf(err, err_len, "%s: states must be an array", where);
        return false;
    }
    const cJSON *e;
    cJSON_ArrayForEach(e, js) {
        if (w->state_count >= CFG_MAX_STATES) {
            snprintf(err, err_len, "%s: more than %d states", where, CFG_MAX_STATES);
            return false;
        }
        state_map_t *m = &w->states[w->state_count];
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(e, "value");
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(e, "text");
        const cJSON *state = cJSON_GetObjectItemCaseSensitive(e, "state");
        if (!cJSON_IsString(value) || value->valuestring[0] == '\0') {
            snprintf(err, err_len, "%s: state %d needs a value (as text, e.g. \"bulk\", \"0\", \"true\")", where,
                     w->state_count);
            return false;
        }
        if (!check_len(value, CFG_STATE_LEN, "state value", where, err, err_len) ||
            !check_len(text, CFG_STATE_LEN, "state text", where, err, err_len)) {
            return false;
        }
        for (int i = 0; i < w->state_count; i++) {
            if (strcmp(w->states[i].value, value->valuestring) == 0) {
                snprintf(err, err_len, "%s: state value '%s' listed twice", where, value->valuestring);
                return false;
            }
        }
        copy_str(m->value, sizeof(m->value), value, "");
        copy_str(m->text, sizeof(m->text), text, "");
        m->state = ZONE_NORMAL;
        if (state && (!cJSON_IsString(state) || !zone_state_from_name(state->valuestring, &m->state))) {
            snprintf(err, err_len, "%s: state %d: state must be nominal, normal, alert, warn, alarm or emergency",
                     where, w->state_count);
            return false;
        }
        w->state_count++;
    }
    return true;
}

// alert: bind.notifications.prefix, a path under notifications. ("" or absent = all of them).
static bool parse_notifications(const cJSON *jw, widget_cfg_t *w, const char *where, char *err, int err_len)
{
    const cJSON *n = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(jw, "bind"), "notifications");
    const cJSON *prefix = cJSON_GetObjectItemCaseSensitive(n, "prefix");
    w->notify_prefix[0] = '\0';
    if (!prefix) return true;
    if (!cJSON_IsString(prefix)) {
        snprintf(err, err_len, "%s: notifications.prefix must be text", where);
        return false;
    }
    const char *p = prefix->valuestring;
    if (strncmp(p, "notifications.", 14) == 0) p += 14;   // either form is fine
    if (strcmp(p, "notifications") == 0) p = "";
    if (strlen(p) >= CFG_PATH_LEN - 16) {   // room for "notifications." and ".*" in the subscription
        snprintf(err, err_len, "%s: notifications.prefix is too long", where);
        return false;
    }
    if (p[0] == '.' || (p[0] && p[strlen(p) - 1] == '.') || strchr(p, '*')) {
        snprintf(err, err_len, "%s: notifications.prefix must be a path like electrical.batteries (no wildcards)",
                 where);
        return false;
    }
    snprintf(w->notify_prefix, sizeof(w->notify_prefix), "%s", p);
    return true;
}

#define TIDE_HOURS_DEFAULT  18
#define TIDE_HOURS_MIN      6
#define TIDE_HOURS_MAX      48   // the fetch window must fit tides.h's TIDES_MAX_POINTS

// tide: where to fetch predictions (a path on the Signal K server) and how far ahead to show.
static bool parse_tide(const cJSON *jw, widget_cfg_t *w, const char *where, char *err, int err_len)
{
    const cJSON *api = cJSON_GetObjectItemCaseSensitive(jw, "api");
    if (!cJSON_IsString(api) || api->valuestring[0] != '/') {
        snprintf(err, err_len, "%s: needs api, a path on the Signal K server starting with /", where);
        return false;
    }
    if (!check_len(api, CFG_PATH_LEN, "api", where, err, err_len)) return false;
    copy_str(w->api, sizeof(w->api), api, "");
    w->hours = get_int(jw, "hours", TIDE_HOURS_DEFAULT);
    if (w->hours < TIDE_HOURS_MIN || w->hours > TIDE_HOURS_MAX) {
        snprintf(err, err_len, "%s: hours must be %d-%d", where, TIDE_HOURS_MIN, TIDE_HOURS_MAX);
        return false;
    }
    return true;
}

// bind.<name>.path into dst. Absent is fine unless required.
static bool parse_bind_path(const cJSON *jw, const char *name, bool required, char *dst, const char *where,
                            char *err, int err_len)
{
    const cJSON *b = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(jw, "bind"), name);
    const cJSON *path = cJSON_GetObjectItemCaseSensitive(b, "path");
    dst[0] = '\0';
    if (!b && !required) return true;
    if (!cJSON_IsString(path) || path->valuestring[0] == '\0') {
        snprintf(err, err_len, "%s: no bind.%s.path", where, name);
        return false;
    }
    if (!check_len(path, CFG_PATH_LEN, name, where, err, err_len)) return false;
    copy_str(dst, CFG_PATH_LEN, path, "");
    return true;
}

#define TRAIL_MIN_DEFAULT  240
#define TRAIL_MIN_MIN      10
#define TRAIL_MIN_MAX      720   // 12 h: a night at anchor

static bool parse_anchor(const cJSON *jw, widget_cfg_t *w, const char *where, char *err, int err_len)
{
    if (!parse_bind_path(jw, "position", true, w->pos_path, where, err, err_len) ||
        !parse_bind_path(jw, "anchor", true, w->anchor_path, where, err, err_len) ||
        !parse_bind_path(jw, "radius", true, w->radius_path, where, err, err_len) ||
        !parse_bind_path(jw, "heading", false, w->heading_path, where, err, err_len)) {
        return false;
    }
    const cJSON *history = cJSON_GetObjectItemCaseSensitive(jw, "history");
    if (history && (!cJSON_IsString(history) || history->valuestring[0] != '/')) {
        snprintf(err, err_len, "%s: history must be a path on the Signal K server starting with /", where);
        return false;
    }
    if (!check_len(history, CFG_PATH_LEN, "history", where, err, err_len)) return false;
    copy_str(w->api, sizeof(w->api), history, "");
    w->trail_min = get_int(jw, "trail_minutes", TRAIL_MIN_DEFAULT);
    if (w->trail_min < TRAIL_MIN_MIN || w->trail_min > TRAIL_MIN_MAX) {
        snprintf(err, err_len, "%s: trail_minutes must be %d-%d", where, TRAIL_MIN_MIN, TRAIL_MIN_MAX);
        return false;
    }
    return true;
}

static bool parse_widget(const cJSON *jw, int index, const screen_cfg_t *screen, widget_cfg_t *w,
                         char *err, int err_len)
{
    char where[CFG_TEXT_LEN + 32];
    snprintf(where, sizeof(where), "screen '%s' widget %d", screen->id, index);

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(jw, "type");
    if (!cJSON_IsString(type)) {
        snprintf(err, err_len, "%s: no type", where);
        return false;
    }
    w->type = widget_type_find(type->valuestring);
    if (w->type < 0) {
        snprintf(err, err_len, "%s: unknown widget type '%s'", where, type->valuestring);
        return false;
    }
    const widget_type_t *t = &widget_types[w->type];

    const cJSON *at = cJSON_GetObjectItemCaseSensitive(jw, "at");
    if (!cJSON_IsObject(at)) {
        snprintf(err, err_len, "%s: no 'at' position", where);
        return false;
    }
    w->col = get_int(at, "col", 0);
    w->row = get_int(at, "row", 0);
    w->w = get_int(at, "w", 1);
    w->h = get_int(at, "h", 1);
    if (w->col < 0 || w->row < 0 || w->w < 1 || w->h < 1 ||
        w->col + w->w > screen->cols || w->row + w->h > screen->rows) {
        snprintf(err, err_len, "%s: outside the %dx%d grid", where, screen->cols, screen->rows);
        return false;
    }

    const cJSON *label = cJSON_GetObjectItemCaseSensitive(jw, "label");
    if (!check_len(label, CFG_TEXT_LEN, "label", where, err, err_len)) return false;
    copy_str(w->label, sizeof(w->label), label, "");
    if (!t->takes_value && !t->takes_notifications && w->label[0] == '\0') {
        snprintf(err, err_len, "%s: %s needs a label", where, t->name);
        return false;
    }

    if (t->takes_range && !parse_range(jw, w, where, err, err_len)) return false;
    if (t->takes_orientation && !parse_orientation(jw, w, where, err, err_len)) return false;
    if (t->takes_states && !parse_states(jw, w, where, err, err_len)) return false;
    if (t->takes_notifications && !parse_notifications(jw, w, where, err, err_len)) return false;
    if (t->takes_tide && !parse_tide(jw, w, where, err, err_len)) return false;
    if (t->takes_anchor && !parse_anchor(jw, w, where, err, err_len)) return false;

    const cJSON *value = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(jw, "bind"), "value");
    if (t->takes_value && (value || t->value_required)) {
        return parse_value_binding(value, &w->value, where, err, err_len);
    }
    return true;
}

// Two widgets in the same cell would draw over each other.
static bool check_overlap(const screen_cfg_t *s, char *err, int err_len)
{
    for (int i = 0; i < s->widget_count; i++) {
        for (int j = i + 1; j < s->widget_count; j++) {
            const widget_cfg_t *a = &s->widgets[i], *b = &s->widgets[j];
            if (a->col < b->col + b->w && b->col < a->col + a->w &&
                a->row < b->row + b->h && b->row < a->row + a->h) {
                snprintf(err, err_len, "screen '%s': widgets %d and %d overlap", s->id, i, j);
                return false;
            }
        }
    }
    return true;
}

static bool parse_screen(const cJSON *js, int index, screen_cfg_t *s, char *err, int err_len)
{
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(js, "id");
    const cJSON *title = cJSON_GetObjectItemCaseSensitive(js, "title");
    if (!cJSON_IsString(id) || id->valuestring[0] == '\0') {
        snprintf(err, err_len, "screen %d: no id", index);
        return false;
    }
    if (!check_len(id, CFG_TEXT_LEN, "id", "screen", err, err_len) ||
        !check_len(title, CFG_TEXT_LEN, "title", id->valuestring, err, err_len)) return false;
    copy_str(s->id, sizeof(s->id), id, "");
    copy_str(s->title, sizeof(s->title), title, s->id);

    const cJSON *grid = cJSON_GetObjectItemCaseSensitive(js, "grid");
    s->cols = get_int(grid, "cols", 4);
    s->rows = get_int(grid, "rows", 3);
    if (s->cols < 1 || s->rows < 1 || s->cols > 24 || s->rows > 24) {
        snprintf(err, err_len, "screen '%s': grid must be 1-24 cols and rows", s->id);
        return false;
    }

    s->widget_count = 0;
    const cJSON *jw;
    cJSON_ArrayForEach(jw, cJSON_GetObjectItemCaseSensitive(js, "widgets")) {
        if (s->widget_count >= CFG_MAX_WIDGETS) {
            snprintf(err, err_len, "screen '%s': more than %d widgets", s->id, CFG_MAX_WIDGETS);
            return false;
        }
        if (!parse_widget(jw, s->widget_count, s, &s->widgets[s->widget_count], err, err_len)) return false;
        s->widget_count++;
    }
    return check_overlap(s, err, err_len);
}

static int screen_index(const config_t *cfg, const char *id)
{
    for (int i = 0; i < cfg->screen_count; i++) {
        if (strcmp(cfg->screens[i].id, id) == 0) return i;
    }
    return -1;
}

static bool parse_auto_screen(const cJSON *ja, config_t *cfg, char *err, int err_len)
{
    auto_screen_t *a = &cfg->auto_screen;
    if (!ja) return true;
    const cJSON *path = cJSON_GetObjectItemCaseSensitive(ja, "path");
    if (!cJSON_IsString(path) || path->valuestring[0] == '\0') {
        snprintf(err, err_len, "auto_screen: no path");
        return false;
    }
    if (!check_len(path, CFG_PATH_LEN, "path", "auto_screen", err, err_len)) return false;
    copy_str(a->path, sizeof(a->path), path, "");
    a->manual_hold_s = get_int(ja, "manual_hold_s", 300);
    if (a->manual_hold_s < 0) {
        snprintf(err, err_len, "auto_screen: manual_hold_s must be 0 or more");
        return false;
    }
    const cJSON *map = cJSON_GetObjectItemCaseSensitive(ja, "screens");
    if (!cJSON_IsObject(map)) {
        snprintf(err, err_len, "auto_screen: screens must map values to screen ids");
        return false;
    }
    const cJSON *m;
    cJSON_ArrayForEach(m, map) {
        if (a->count >= CFG_MAX_AUTO) {
            snprintf(err, err_len, "auto_screen: more than %d values", CFG_MAX_AUTO);
            return false;
        }
        if (strlen(m->string) >= CFG_STATE_LEN) {
            snprintf(err, err_len, "auto_screen: value '%s' is longer than %d characters", m->string, CFG_STATE_LEN - 1);
            return false;
        }
        int index = cJSON_IsString(m) ? screen_index(cfg, m->valuestring) : -1;
        if (index < 0) {
            snprintf(err, err_len, "auto_screen: '%s' maps to an unknown screen", m->string);
            return false;
        }
        snprintf(a->states[a->count], CFG_STATE_LEN, "%s", m->string);
        a->screens[a->count++] = index;
    }
    return true;
}

static bool valid_hostname(const char *h)
{
    if (h[0] == '\0' || h[0] == '-') return false;
    for (const char *c = h; *c; c++) {
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-')) return false;
    }
    return true;
}

bool config_parse(const char *json, config_t *cfg, char *err, int err_len)
{
    memset(cfg, 0, sizeof(*cfg));
    if (strlen(json) > CFG_MAX_JSON) {
        snprintf(err, err_len, "config is larger than %d bytes", CFG_MAX_JSON);
        return false;
    }
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        const char *at = cJSON_GetErrorPtr();
        snprintf(err, err_len, "config is not valid JSON (near byte %d)", at ? (int)(at - json) : 0);
        return false;
    }

    bool ok = false;
    cfg->schema_version = get_int(root, "schema_version", 0);
    if (cfg->schema_version != SCHEMA_VERSION) {
        snprintf(err, err_len, "schema_version %d not supported (expected %d)", cfg->schema_version, SCHEMA_VERSION);
        goto done;
    }

    const cJSON *device = cJSON_GetObjectItemCaseSensitive(root, "device");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(device, "name");
    const cJSON *hostname = cJSON_GetObjectItemCaseSensitive(device, "hostname");
    if (!check_len(name, sizeof(cfg->device_name), "name", "device", err, err_len) ||
        !check_len(hostname, sizeof(cfg->hostname), "hostname", "device", err, err_len)) goto done;
    copy_str(cfg->device_name, sizeof(cfg->device_name), name, "Telltale");
    copy_str(cfg->hostname, sizeof(cfg->hostname), hostname, "telltale");
    if (!valid_hostname(cfg->hostname)) {
        snprintf(err, err_len, "device: hostname may only use a-z, 0-9 and '-'");
        goto done;
    }

    const cJSON *sk = cJSON_GetObjectItemCaseSensitive(root, "signalk");
    const cJSON *host = cJSON_GetObjectItemCaseSensitive(sk, "host");
    if (!check_len(host, sizeof(cfg->sk_host), "host", "signalk", err, err_len)) goto done;
    copy_str(cfg->sk_host, sizeof(cfg->sk_host), host, "");
    cfg->sk_port = get_int(sk, "port", 80);
    if (cfg->sk_port < 1 || cfg->sk_port > 65535) {
        snprintf(err, err_len, "signalk: bad port");
        goto done;
    }

    const cJSON *clock = cJSON_GetObjectItemCaseSensitive(root, "clock");
    const cJSON *offset = cJSON_GetObjectItemCaseSensitive(clock, "offset_path");
    const cJSON *hour12 = cJSON_GetObjectItemCaseSensitive(clock, "hour12");
    if ((offset && !cJSON_IsString(offset)) || (hour12 && !cJSON_IsBool(hour12))) {
        snprintf(err, err_len, "clock: offset_path must be text and hour12 true or false");
        goto done;
    }
    if (!check_len(offset, sizeof(cfg->clock_offset_path), "offset_path", "clock", err, err_len)) goto done;
    copy_str(cfg->clock_offset_path, sizeof(cfg->clock_offset_path), offset, "");
    cfg->clock_hour12 = cJSON_IsTrue(hour12);

    const cJSON *js;
    cJSON_ArrayForEach(js, cJSON_GetObjectItemCaseSensitive(root, "screens")) {
        if (cfg->screen_count >= CFG_MAX_SCREENS) {
            snprintf(err, err_len, "more than %d screens", CFG_MAX_SCREENS);
            goto done;
        }
        screen_cfg_t *s = &cfg->screens[cfg->screen_count];
        if (!parse_screen(js, cfg->screen_count, s, err, err_len)) goto done;
        for (int i = 0; i < cfg->screen_count; i++) {
            if (strcmp(cfg->screens[i].id, s->id) == 0) {
                snprintf(err, err_len, "two screens with id '%s'", s->id);
                goto done;
            }
        }
        cfg->screen_count++;
    }
    if (cfg->screen_count == 0) {
        snprintf(err, err_len, "config has no screens");
        goto done;
    }
    if (!parse_auto_screen(cJSON_GetObjectItemCaseSensitive(root, "auto_screen"), cfg, err, err_len)) goto done;
    ok = true;

done:
    cJSON_Delete(root);
    return ok;
}
