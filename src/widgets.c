#include "widgets.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "alerts.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "store.h"
#include "fonts.h"
#include "theme.h"
#include "sk_time.h"
#include "tides.h"
#include "history.h"
#include "track.h"
#include "units.h"
#include "widget_types.h"

static const char *TAG = "widgets";

typedef struct {
    const char *name;
    void (*create)(widget_view_t *v, lv_obj_t *card);
    void (*update)(widget_view_t *v);
} widget_ops_t;

static const lv_font_t *value_font(lv_coord_t h)
{
    if (h >= 150) return &lv_font_montserrat_48;
    if (h >= 100) return &lv_font_montserrat_32;
    return &lv_font_montserrat_24;
}

// Big readings (value, state), largest first. The largest that fits the card is used, so a
// long one ("Not charging", a position) gets smaller instead of running off the card.
static const lv_font_t *const READING_FONTS[] = { &reading_96, &reading_64, &reading_48, &reading_36,
                                                  &lv_font_montserrat_24, &lv_font_montserrat_16 };
#define READING_FONT_COUNT (sizeof(READING_FONTS) / sizeof(READING_FONTS[0]))

static const lv_font_t *fit_font(const char *text, lv_coord_t w, lv_coord_t h)
{
    for (size_t i = 0; i < READING_FONT_COUNT; i++) {
        lv_point_t size;
        lv_txt_get_size(&size, text, READING_FONTS[i], 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (size.x <= w && size.y <= h) return READING_FONTS[i];
    }
    return READING_FONTS[READING_FONT_COUNT - 1];
}

static lv_color_t zone_color(zone_state_t z)
{
    switch (z) {
    case ZONE_ALERT:
    case ZONE_WARN:      return COLOR_WARN;
    case ZONE_ALARM:
    case ZONE_EMERGENCY: return COLOR_ALARM;
    default:             return COLOR_TEXT;
    }
}

void ui_set_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

void ui_set_text_color(lv_obj_t *obj, lv_color_t color)
{
    if (lv_obj_get_style_text_color(obj, LV_PART_MAIN).full != color.full) {
        lv_obj_set_style_text_color(obj, color, 0);
    }
}

// ---- label ----

static void label_create(widget_view_t *v, lv_obj_t *card)
{
    v->label = lv_label_create(card);
    lv_label_set_text(v->label, v->cfg->label);
    lv_obj_set_style_text_color(v->label, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(v->label, &lv_font_montserrat_24, 0);
    lv_obj_align(v->label, LV_ALIGN_CENTER, 0, 0);
}

// ---- shared by the value-bound types ----

// A bound value as it should look now.
typedef struct {
    bool have;          // a value has arrived (else "--")
    bool is_number;
    double si;          // when is_number
    char text[40];      // formatted with unit (numbers) or as received (text)
    char number[40];    // the same without the unit
    char unit[16];      // the unit symbol, "" for text
    lv_color_t color;   // zone colour; COLOR_STALE when stale or missing
    zone_state_t zone;
} reading_t;

// Converts an SI number for display: config unit and decimals win; then Signal K's display
// settings; then SI as received. Returns the shown number; `text` gets it with the unit symbol,
// `number` and `unit` (may be NULL) get them apart.
static double format_parts(const binding_t *b, const store_meta_t *meta, double si, char *text, int len,
                           char *number, int number_len, char *unit_out, int unit_len)
{
    const char *unit = b->unit[0] ? b->unit : meta->unit;
    int decimals = b->decimals != CFG_DECIMALS_AUTO ? b->decimals : (meta->decimals >= 0 ? meta->decimals : 1);
    double shown;
    const char *symbol;
    if (unit[0] && units_convert(unit, si, &shown)) {
        symbol = units_symbol(unit);
    } else {
        shown = si;
        symbol = units_symbol(meta->si);
    }
    // newlib snprintf: LVGL's own formatter has float support off by default
    snprintf(text, len, "%.*f %s", decimals, shown, symbol);
    // Degrees stay on the number ("314°"); a lone ° in a corner reads as a speck.
    bool deg = strcmp(symbol, units_symbol("deg")) == 0;
    if (number) snprintf(number, number_len, "%.*f%s", decimals, shown, deg ? symbol : "");
    if (unit_out) snprintf(unit_out, unit_len, "%s", deg ? "" : symbol);
    return shown;
}

static double format_number(const binding_t *b, const store_meta_t *meta, double si, char *text, int len)
{
    return format_parts(b, meta, si, text, len, NULL, 0, NULL, 0);
}

// Reads the widget's binding from the store. Also keeps a defaulted label in step with the
// metadata displayName. False if the path isn't watched (nothing to draw).
static bool read_binding(widget_view_t *v, reading_t *r)
{
    const binding_t *b = &v->cfg->value;
    store_value_t val;
    store_meta_t meta;
    if (!store_get(b->path, &val, &meta)) return false;

    if (!v->cfg->label[0] && meta.display_name[0]) ui_set_text(v->label, meta.display_name);

    *r = (reading_t){ .color = COLOR_STALE, .zone = ZONE_NONE };
    snprintf(r->text, sizeof(r->text), "--");
    snprintf(r->number, sizeof(r->number), "--");
    if (val.kind == VAL_NONE) return true;

    r->have = true;
    bool stale = val.updated_us == 0 ||
                 (esp_timer_get_time() - val.updated_us) > (int64_t)b->stale_after_s * 1000000;
    if (val.kind == VAL_TEXT) {
        snprintf(r->text, sizeof(r->text), "%s", val.text);
        snprintf(r->number, sizeof(r->number), "%s", val.text);
    } else if (val.kind == VAL_POSITION) {
        // Degrees and decimal minutes, as on a chartplotter: 48°59.31'N 122°46.15'W
        double la = fabs(val.lat), lo = fabs(val.lon);
        snprintf(r->text, sizeof(r->text), "%d\xC2\xB0%05.2f'%c %d\xC2\xB0%05.2f'%c", (int)la, (la - (int)la) * 60,
                 val.lat < 0 ? 'S' : 'N', (int)lo, (lo - (int)lo) * 60, val.lon < 0 ? 'W' : 'E');
        snprintf(r->number, sizeof(r->number), "%s", r->text);
        char *gap = strchr(r->number, ' ');
        if (gap) *gap = '\n';   // latitude over longitude, as a chartplotter shows it
    } else {
        r->is_number = true;
        r->si = val.number;
        format_parts(b, &meta, val.number, r->text, sizeof(r->text), r->number, sizeof(r->number),
                     r->unit, sizeof(r->unit));

        const zone_t *zones = b->zone_count ? b->zones : meta.zones;
        int zone_count = b->zone_count ? b->zone_count : meta.zone_count;
        r->zone = zones_state(zones, zone_count, val.number);
    }
    r->color = stale ? COLOR_STALE : zone_color(r->zone);
    return true;
}

static lv_coord_t content_w(lv_obj_t *card)
{
    return lv_obj_get_style_width(card, 0) - lv_obj_get_style_pad_left(card, 0) - lv_obj_get_style_pad_right(card, 0);
}

static lv_coord_t content_h(lv_obj_t *card)
{
    return lv_obj_get_style_height(card, 0) - lv_obj_get_style_pad_top(card, 0) - lv_obj_get_style_pad_bottom(card, 0);
}

// The small caption at the top left: config label, else (until metadata brings a displayName)
// the path's last element.
static void create_caption(widget_view_t *v, lv_obj_t *card)
{
    v->label = lv_label_create(card);
    lv_obj_set_style_text_color(v->label, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(v->label, &lv_font_montserrat_16, 0);
    lv_obj_align(v->label, LV_ALIGN_TOP_LEFT, 0, 0);
    if (v->cfg->label[0]) {
        lv_label_set_text(v->label, v->cfg->label);
    } else {
        const char *dot = strrchr(v->cfg->value.path, '.');
        lv_label_set_text(v->label, dot ? dot + 1 : v->cfg->value.path);
    }
}

static lv_obj_t *create_value_text(lv_obj_t *parent, const lv_font_t *font)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, "--");
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, COLOR_STALE, 0);
    return l;
}

// Position of the value on the gauge scale, 0..GAUGE_STEPS (clamped).
#define GAUGE_STEPS 1000
static int32_t gauge_pos(const widget_cfg_t *c, const reading_t *r)
{
    if (!r->have || !r->is_number) return 0;
    double f = (r->si - c->min) / (c->max - c->min);
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    return (int32_t)(f * GAUGE_STEPS + 0.5);
}

// Zone colour on the gauge; the accent when the value is in no warning zone.
static lv_color_t gauge_color(const reading_t *r)
{
    if (r->color.full == COLOR_STALE.full) return COLOR_STALE;
    return r->zone >= ZONE_ALERT ? r->color : COLOR_ACCENT;
}

// ---- value ----

// The value and state layout: caption top left, the reading big in the middle, the unit small
// in the bottom-right corner.
static void reading_create(widget_view_t *v, lv_obj_t *card)
{
    create_caption(v, card);
    v->value = create_value_text(card, &reading_36);
    lv_obj_set_style_text_align(v->value, LV_TEXT_ALIGN_CENTER, 0);
    v->unit = lv_label_create(card);
    lv_label_set_text(v->unit, "");
    lv_obj_set_style_text_font(v->unit, content_h(card) >= 150 ? &lv_font_montserrat_24 : &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(v->unit, COLOR_STALE, 0);
    lv_obj_align(v->unit, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
}

static void show_reading(widget_view_t *v, const char *number, const char *unit, lv_color_t color)
{
    lv_obj_t *card = v->card;
    lv_coord_t cap = lv_font_get_line_height(&lv_font_montserrat_16);
    lv_coord_t unit_h = unit[0] ? lv_font_get_line_height(lv_obj_get_style_text_font(v->unit, 0)) : 0;
    // The reading gets the space between the caption and the unit, centred in it.
    const lv_font_t *font = fit_font(number, content_w(card), content_h(card) - cap - unit_h);
    if (lv_obj_get_style_text_font(v->value, 0) != font) lv_obj_set_style_text_font(v->value, font, 0);
    lv_coord_t dy = (cap - unit_h) / 2;
    if (lv_obj_get_style_y(v->value, 0) != dy || lv_obj_get_style_align(v->value, 0) != LV_ALIGN_CENTER) {
        lv_obj_align(v->value, LV_ALIGN_CENTER, 0, dy);
    }
    ui_set_text(v->value, number);
    ui_set_text(v->unit, unit);
    ui_set_text_color(v->value, color);
    ui_set_text_color(v->unit, color);
}

static void value_create(widget_view_t *v, lv_obj_t *card) { reading_create(v, card); }

static void value_update(widget_view_t *v)
{
    reading_t r;
    if (!read_binding(v, &r)) return;
    show_reading(v, r.number, r.unit, r.color);
}

// ---- ring: 270-degree arc, value in the middle ----

static void ring_create(widget_view_t *v, lv_obj_t *card)
{
    create_caption(v, card);
    lv_coord_t cw = content_w(card), ch = content_h(card) - 20;   // room for the caption
    lv_coord_t d = cw < ch ? cw : ch;

    lv_obj_t *arc = lv_arc_create(card);
    lv_obj_set_size(arc, d, d);
    lv_obj_align(arc, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_range(arc, 0, GAUGE_STEPS);
    lv_arc_set_value(arc, 0);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);   // display only; swipes go to the screen
    lv_coord_t thick = d / 10 < 6 ? 6 : d / 10;
    lv_obj_set_style_arc_width(arc, thick, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, thick, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, COLOR_TRACK, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, COLOR_STALE, LV_PART_INDICATOR);
    v->gauge = arc;

    v->value = create_value_text(arc, &reading_36);
    lv_obj_center(v->value);
    v->unit = lv_label_create(arc);
    lv_label_set_text(v->unit, "");
    lv_obj_set_style_text_font(v->unit, d >= 140 ? &lv_font_montserrat_24 : &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(v->unit, COLOR_STALE, 0);
}

static void ring_update(widget_view_t *v)
{
    reading_t r;
    if (!read_binding(v, &r)) return;
    // The number as big as fits inside the ring, the unit under it.
    lv_coord_t d = lv_obj_get_style_width(v->gauge, 0);
    lv_coord_t inner = d - 2 * lv_obj_get_style_arc_width(v->gauge, LV_PART_MAIN);
    const lv_font_t *font = fit_font(r.number, inner * 3 / 4, inner / 2);
    if (lv_obj_get_style_text_font(v->value, 0) != font) lv_obj_set_style_text_font(v->value, font, 0);
    ui_set_text(v->value, r.number);
    ui_set_text(v->unit, r.unit);
    lv_obj_align_to(v->unit, v->value, LV_ALIGN_OUT_BOTTOM_MID, 0, -4);
    ui_set_text_color(v->value, r.color);
    ui_set_text_color(v->unit, r.color);
    int32_t pos = gauge_pos(v->cfg, &r);
    if (lv_arc_get_value(v->gauge) != pos) lv_arc_set_value(v->gauge, pos);
    lv_color_t c = gauge_color(&r);
    if (lv_obj_get_style_arc_color(v->gauge, LV_PART_INDICATOR).full != c.full) {
        lv_obj_set_style_arc_color(v->gauge, c, LV_PART_INDICATOR);
    }
}

// ---- bar: horizontal (caption and value above) or vertical (bar on the left) ----

static void bar_create(widget_view_t *v, lv_obj_t *card)
{
    create_caption(v, card);
    lv_coord_t cw = content_w(card), ch = content_h(card);
    lv_obj_t *bar = lv_bar_create(card);
    lv_bar_set_range(bar, 0, GAUGE_STEPS);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    // The whole track shows, so a part-full bar reads as part-full and not as a short bar.
    lv_obj_set_style_bg_color(bar, COLOR_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, COLOR_STALE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    v->gauge = bar;

    if (v->cfg->vertical) {
        lv_coord_t bw = cw / 4 < 14 ? 14 : cw / 4;
        lv_obj_set_size(bar, bw, ch);   // LVGL fills it bottom-up because it is taller than wide
        lv_obj_align(bar, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_align(v->label, LV_ALIGN_TOP_LEFT, bw + 8, 0);
        v->value = create_value_text(card, value_font(lv_obj_get_style_height(card, 0) / 2));
        lv_obj_align(v->value, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    } else {
        // Value on the top line; bar centred in the space below it.
        lv_coord_t bh = ch / 5 < 14 ? 14 : ch / 5;
        const lv_font_t *font = value_font(lv_obj_get_style_height(card, 0) - bh);
        lv_coord_t text_h = lv_font_get_line_height(font);
        lv_coord_t y = text_h + (ch - text_h - bh) / 2;
        lv_obj_set_size(bar, cw, bh);
        lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, y > text_h ? y : text_h);
        v->value = create_value_text(card, font);
        lv_obj_align(v->value, LV_ALIGN_TOP_RIGHT, 0, 0);
    }
}

static void bar_update(widget_view_t *v)
{
    reading_t r;
    if (!read_binding(v, &r)) return;
    ui_set_text(v->value, r.text);
    ui_set_text_color(v->value, r.color);
    int32_t pos = gauge_pos(v->cfg, &r);
    if (lv_bar_get_value(v->gauge) != pos) lv_bar_set_value(v->gauge, pos, LV_ANIM_OFF);
    lv_color_t c = gauge_color(&r);
    if (lv_obj_get_style_bg_color(v->gauge, LV_PART_INDICATOR).full != c.full) {
        lv_obj_set_style_bg_color(v->gauge, c, LV_PART_INDICATOR);
    }
}

// ---- state: an enum shown as text and colour from the config's states map ----

static void state_create(widget_view_t *v, lv_obj_t *card) { reading_create(v, card); }

static void state_update(widget_view_t *v)
{
    reading_t r;
    if (!read_binding(v, &r)) return;
    const widget_cfg_t *c = v->cfg;
    char key[sizeof(r.text)];
    if (r.is_number) {
        snprintf(key, sizeof(key), "%g", r.si);   // matched as written in the config: "0", "1.5"
    } else {
        snprintf(key, sizeof(key), "%s", r.text);
    }
    const char *shown = r.text;
    lv_color_t color = r.color;
    if (r.have) {
        if (r.is_number) shown = key;   // an enum, not a measurement: no unit
        for (int i = 0; i < c->state_count; i++) {
            if (strcmp(c->states[i].value, key) == 0) {
                if (c->states[i].text[0]) shown = c->states[i].text;
                if (color.full != COLOR_STALE.full) color = zone_color(c->states[i].state);
                break;
            }
        }
    }
    show_reading(v, shown, "", color);
}

// ---- alert: the most severe active notification under a prefix ----

static void alert_create(widget_view_t *v, lv_obj_t *card)
{
    v->label = lv_label_create(card);
    lv_obj_set_style_text_color(v->label, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(v->label, &lv_font_montserrat_16, 0);
    lv_obj_align(v->label, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text(v->label, v->cfg->label[0] ? v->cfg->label : "Alerts");

    // Messages are sentences: wrap, and cut with "..." if they still don't fit.
    lv_coord_t line = lv_font_get_line_height(&lv_font_montserrat_16);
    const lv_font_t *font = lv_obj_get_style_height(card, 0) >= 200 ? &lv_font_montserrat_32 : &lv_font_montserrat_24;
    v->value = create_value_text(card, font);
    lv_label_set_long_mode(v->value, LV_LABEL_LONG_DOT);
    lv_obj_set_size(v->value, content_w(card), content_h(card) - 2 * line);
    lv_obj_set_style_text_align(v->value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(v->value, LV_ALIGN_TOP_MID, 0, line);

    v->gauge = lv_label_create(card);
    lv_label_set_text(v->gauge, "");
    lv_obj_set_style_text_font(v->gauge, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(v->gauge, COLOR_MUTED, 0);
    lv_obj_align(v->gauge, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
}

static void alert_update(widget_view_t *v)
{
    char text[CFG_PATH_LEN], more[24] = "";   // the message, or the path when there is none
    lv_color_t fg, small = COLOR_MUTED, bg = COLOR_CARD;
    alert_t a;
    if (!alerts_live()) {
        // A dead link must not look like a quiet boat.
        snprintf(text, sizeof(text), "Alerts unknown: no Signal K");
        fg = COLOR_WARN;
    } else {
        int n = alerts_worst(v->cfg->notify_prefix, &a);
        if (n == 0) {
            snprintf(text, sizeof(text), "No alerts");
            fg = COLOR_MUTED;
        } else {
            snprintf(text, sizeof(text), "%s", a.message[0] ? a.message : a.path);
            bg = zone_color(a.state);   // amber or red card, dark text on it
            fg = small = COLOR_BG;
            if (n > 1) snprintf(more, sizeof(more), "+%d more", n - 1);
        }
    }
    ui_set_text(v->value, text);
    ui_set_text_color(v->value, fg);
    ui_set_text(v->gauge, more);
    ui_set_text_color(v->gauge, small);
    ui_set_text_color(v->label, small);
    if (lv_obj_get_style_bg_color(v->card, LV_PART_MAIN).full != bg.full) {
        lv_obj_set_style_bg_color(v->card, bg, 0);
    }
}

// ---- tide: predicted curve, highs and lows labelled, the lowest low called out ----

static char s_clock_path[CFG_PATH_LEN];
static bool s_hour12;

void widgets_set_clock(const char *offset_path, bool hour12)
{
    snprintf(s_clock_path, sizeof(s_clock_path), "%s", offset_path);
    s_hour12 = hour12;
}

// Local clock text for `ms`; "UTC" appended when there is no offset to apply.
static void clock_text(int64_t ms, char *buf, int len)
{
    store_value_t v;
    int offset;
    if (s_clock_path[0] && store_get(s_clock_path, &v, NULL) && v.kind == VAL_NUMBER &&
        sk_time_offset_minutes(v.number, &offset)) {
        sk_time_format_clock(ms, offset, s_hour12, buf, len);
    } else {
        char t[16];
        sk_time_format_clock(ms, 0, false, t, sizeof(t));
        snprintf(buf, len, "%s UTC", t);
    }
}

#define TIDE_MARKS     8
#define TIDE_BEHIND_MS (60 * 60 * 1000LL)   // the curve starts an hour back, so "now" isn't at the edge
#define MARK_W         110
#define MARK_ROOM      40                    // two 16 px lines above highs / below lows
#define CURVE_PAD      4                     // keeps the 3 px line's highest and lowest points inside the chart

typedef struct {
    lv_obj_t *chart;
    lv_chart_series_t *series;
    lv_obj_t *dot;
    lv_obj_t *marks[TIDE_MARKS];
    lv_obj_t *footer;
    lv_obj_t *station;         // prediction station, above the footer
    lv_coord_t x0, y0, w, h;   // the curve's box in card coordinates
    unsigned drawn_seq;
    int64_t drawn_minute;
    tides_t data;              // copy of the predictions (PSRAM)
} tide_view_t;

static void tide_create(widget_view_t *v, lv_obj_t *card)
{
    tide_view_t *t = heap_caps_calloc(1, sizeof(tide_view_t), MALLOC_CAP_SPIRAM);
    v->priv = t;
    v->label = lv_label_create(card);
    lv_obj_set_style_text_color(v->label, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(v->label, &lv_font_montserrat_16, 0);
    lv_obj_align(v->label, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text(v->label, v->cfg->label[0] ? v->cfg->label : "Tide");

    v->value = create_value_text(card, &lv_font_montserrat_24);   // height now + trend
    lv_obj_align(v->value, LV_ALIGN_TOP_RIGHT, 0, 0);
    if (!t) return;

    lv_coord_t line = lv_font_get_line_height(&lv_font_montserrat_16);
    t->footer = lv_label_create(card);
    lv_label_set_text(t->footer, "Waiting for tide predictions");
    lv_obj_set_style_text_font(t->footer, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(t->footer, COLOR_MUTED, 0);
    lv_obj_align(t->footer, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    t->station = lv_label_create(card);
    lv_label_set_text(t->station, "");
    lv_obj_set_style_text_font(t->station, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(t->station, COLOR_MUTED, 0);
    lv_obj_align(t->station, LV_ALIGN_BOTTOM_LEFT, 0, -line);

    t->x0 = MARK_W / 2;   // room for a label centred on a high/low at either end
    t->w = content_w(card) - MARK_W;
    t->y0 = lv_font_get_line_height(&lv_font_montserrat_24) + MARK_ROOM;
    t->h = content_h(card) - t->y0 - MARK_ROOM - 2 * line - 4;
    if (t->h < 20) t->h = 20;

    t->chart = lv_chart_create(card);
    lv_obj_set_pos(t->chart, t->x0, t->y0);
    lv_obj_set_size(t->chart, t->w, t->h);
    lv_obj_set_style_bg_opa(t->chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(t->chart, 0, 0);
    lv_obj_set_style_pad_all(t->chart, 0, 0);
    lv_obj_set_style_pad_ver(t->chart, CURVE_PAD, 0);
    lv_obj_set_style_line_width(t->chart, 3, LV_PART_ITEMS);
    lv_obj_set_style_size(t->chart, 0, LV_PART_INDICATOR);   // no point markers, just the line
    lv_chart_set_div_line_count(t->chart, 0, 0);
    lv_chart_set_type(t->chart, LV_CHART_TYPE_LINE);
    lv_obj_clear_flag(t->chart, LV_OBJ_FLAG_CLICKABLE);
    t->series = lv_chart_add_series(t->chart, COLOR_ACCENT, LV_CHART_AXIS_PRIMARY_Y);
    lv_obj_add_flag(t->chart, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < TIDE_MARKS; i++) {
        lv_obj_t *m = lv_label_create(card);
        lv_obj_set_width(m, MARK_W);
        lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(m, &lv_font_montserrat_16, 0);
        lv_obj_add_flag(m, LV_OBJ_FLAG_HIDDEN);
        t->marks[i] = m;
    }
    t->dot = lv_obj_create(card);
    lv_obj_remove_style_all(t->dot);
    lv_obj_set_size(t->dot, 12, 12);
    lv_obj_set_style_radius(t->dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(t->dot, COLOR_TEXT, 0);
    lv_obj_set_style_bg_opa(t->dot, LV_OPA_COVER, 0);
    lv_obj_add_flag(t->dot, LV_OBJ_FLAG_HIDDEN);
}

static void tide_update(widget_view_t *v)
{
    tide_view_t *t = v->priv;
    reading_t r;
    if (!t || !read_binding(v, &r)) return;
    store_value_t hv;
    store_meta_t meta;
    store_get(v->cfg->value.path, &hv, &meta);   // for the unit rules on the curve's numbers

    int64_t now;
    bool have_clock = store_clock_ms(&now);
    bool have_tides = tides_get(&t->data);
    // Height now from the curve, so it agrees with the dot and the arrow (signalk-tides' own
    // heightNow was seen republishing an unchanged value for minutes, 2026-10-28); the binding
    // is the fallback before predictions arrive.
    float lv_now, lv_next;
    bool on_curve = have_clock && have_tides && tides_level_at(&t->data, now, &lv_now) &&
                    tides_level_at(&t->data, now + 10 * 60000, &lv_next);
    char head[56], h_now[40];
    if (on_curve) format_number(&v->cfg->value, &meta, lv_now, h_now, sizeof(h_now));
    snprintf(head, sizeof(head), "%s%s", on_curve ? h_now : r.text,
             !on_curve ? "" : lv_next > lv_now ? "  " LV_SYMBOL_UP : lv_next < lv_now ? "  " LV_SYMBOL_DOWN : "");
    ui_set_text(v->value, head);
    ui_set_text_color(v->value, on_curve ? COLOR_TEXT : r.color);

    if (!have_clock || !have_tides) {
        lv_obj_add_flag(t->chart, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(t->dot, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < TIDE_MARKS; i++) lv_obj_add_flag(t->marks[i], LV_OBJ_FLAG_HIDDEN);
        ui_set_text(t->footer, have_clock ? "Waiting for tide predictions" : "Waiting for Signal K");
        ui_set_text(t->station, "");
        t->drawn_seq = 0;
        return;
    }

    // Redraw the curve when the predictions change, and once a minute (now moves; a new timezone
    // offset shows within the minute).
    int64_t minute = now / 60000;
    if (t->drawn_seq == t->data.seq && t->drawn_minute == minute) return;
    t->drawn_seq = t->data.seq;
    t->drawn_minute = minute;

    const binding_t *b = &v->cfg->value;
    int64_t from = now - TIDE_BEHIND_MS, to = now + (int64_t)v->cfg->hours * 3600000LL;
    int n = t->w / 3;
    if (n > 200) n = 200;
    if (n < 2) n = 2;
    char buf[80];
    // Sample the predicted curve across the window, in display units x100.
    static lv_coord_t ys[200];
    lv_coord_t ymin = 32767, ymax = -32768;
    int valid = 0;
    for (int i = 0; i < n; i++) {
        float level;
        int64_t ms = from + (to - from) * i / (n - 1);
        if (!tides_level_at(&t->data, ms, &level)) {
            ys[i] = LV_CHART_POINT_NONE;
            continue;
        }
        double shown = format_number(b, &meta, level, buf, sizeof(buf));
        ys[i] = (lv_coord_t)(shown * 100 + (shown < 0 ? -0.5 : 0.5));
        if (ys[i] < ymin) ymin = ys[i];
        if (ys[i] > ymax) ymax = ys[i];
        valid++;
    }
    if (valid < 2 || ymax <= ymin) return;
    lv_chart_set_point_count(t->chart, n);
    lv_chart_set_range(t->chart, LV_CHART_AXIS_PRIMARY_Y, ymin, ymax);
    for (int i = 0; i < n; i++) lv_chart_set_value_by_id(t->chart, t->series, i, ys[i]);
    lv_chart_refresh(t->chart);
    lv_obj_clear_flag(t->chart, LV_OBJ_FLAG_HIDDEN);

#define X_OF(ms) (t->x0 + (lv_coord_t)((int64_t)t->w * ((ms) - from) / (to - from)))
#define Y_OF(y100) (t->y0 + CURVE_PAD + (lv_coord_t)((int32_t)(t->h - 2 * CURVE_PAD) * (ymax - (y100)) / (ymax - ymin)))

    // Highs and lows in the window, labelled; the lowest low ahead in amber.
    int lowest = tides_lowest(&t->data, now, to);
    int m = 0;
    for (int i = 0; i < t->data.n_ext && m < TIDE_MARKS; i++) {
        const tide_extreme_t *x = &t->data.ext[i];
        if (x->ms < from || x->ms > to) continue;
        char h[24], c[24];
        double shown = format_number(b, &meta, x->level, h, sizeof(h));
        clock_text(x->ms, c, sizeof(c));
        snprintf(buf, sizeof(buf), "%s %s\n%s", x->high ? "H" : "L", h, c);
        lv_obj_t *lbl = t->marks[m++];
        ui_set_text(lbl, buf);
        ui_set_text_color(lbl, i == lowest ? COLOR_WARN : COLOR_MUTED);
        lv_coord_t px = X_OF(x->ms), py = Y_OF((lv_coord_t)(shown * 100));
        lv_obj_set_pos(lbl, px - MARK_W / 2, x->high ? py - MARK_ROOM : py + 6);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_HIDDEN);
    }
    for (; m < TIDE_MARKS; m++) lv_obj_add_flag(t->marks[m], LV_OBJ_FLAG_HIDDEN);

    // Now.
    if (tides_level_at(&t->data, now, &lv_now)) {
        double shown = format_number(b, &meta, lv_now, buf, sizeof(buf));
        lv_obj_set_pos(t->dot, X_OF(now) - 6, Y_OF((lv_coord_t)(shown * 100)) - 6);
        lv_obj_clear_flag(t->dot, LV_OBJ_FLAG_HIDDEN);
    }
#undef X_OF
#undef Y_OF

    if (lowest >= 0) {
        char h[24], c[24];
        format_number(b, &meta, t->data.ext[lowest].level, h, sizeof(h));
        clock_text(t->data.ext[lowest].ms, c, sizeof(c));
        snprintf(buf, sizeof(buf), "Lowest next %d h: %s at %s", v->cfg->hours, h, c);
    } else {
        snprintf(buf, sizeof(buf), "No low in the next %d h", v->cfg->hours);
    }
    ui_set_text(t->footer, buf);
    ui_set_text_color(t->footer, lowest >= 0 ? COLOR_WARN : COLOR_MUTED);
    snprintf(buf, sizeof(buf), "Station: %s", t->data.station);
    ui_set_text(t->station, buf);
}

// ---- anchor: north-up plot of the anchor, alarm circle, boat and swing trail ----

#define TRAIL_DRAW_MAX   300      // points handed to the line; the trail is thinned to fit
#define ANCHOR_REDRAW_MS 2000     // positions arrive about once a second; redraw at most every 2 s
#define VIEW_MIN_M       15.0     // smallest view radius, so a short swing isn't blown up
#define VIEW_MARGIN      1.2      // view radius = what must show x this
#define TRAIL_BANDS      6        // the trail fades by age in this many steps

typedef struct {
    lv_obj_t *plot;               // square; everything below is drawn inside it
    lv_obj_t *circle;
    lv_obj_t *trail[TRAIL_BANDS];   // oldest first
    lv_obj_t *anchor_mark;
    lv_obj_t *boat;
    lv_obj_t *north;
    lv_obj_t *footer;
    lv_coord_t side;
    int64_t drawn_us;
    lv_point_t trail_pts[TRAIL_DRAW_MAX];
    uint8_t trail_band[TRAIL_DRAW_MAX];
    lv_point_t boat_pts[4];
    track_t track;                // the swing trail (PSRAM)
} anchor_view_t;

static void anchor_create(widget_view_t *v, lv_obj_t *card)
{
    anchor_view_t *a = heap_caps_calloc(1, sizeof(anchor_view_t), MALLOC_CAP_SPIRAM);
    v->priv = a;
    v->label = lv_label_create(card);
    lv_obj_set_style_text_color(v->label, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(v->label, &lv_font_montserrat_16, 0);
    lv_obj_align(v->label, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text(v->label, v->cfg->label[0] ? v->cfg->label : "Anchor");
    v->value = create_value_text(card, &lv_font_montserrat_24);   // distance now / circle radius
    lv_obj_align(v->value, LV_ALIGN_TOP_RIGHT, 0, 0);
    if (!a) return;

    lv_coord_t top = lv_font_get_line_height(&lv_font_montserrat_24) + 4;
    lv_coord_t foot = lv_font_get_line_height(&lv_font_montserrat_16) + 4;
    lv_coord_t cw = content_w(card), ch = content_h(card) - top - foot;
    a->side = cw < ch ? cw : ch;

    a->footer = lv_label_create(card);
    lv_label_set_long_mode(a->footer, LV_LABEL_LONG_DOT);
    lv_obj_set_width(a->footer, cw);
    lv_obj_set_style_text_font(a->footer, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(a->footer, COLOR_MUTED, 0);
    lv_obj_align(a->footer, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_label_set_text(a->footer, "Waiting for a position");

    a->plot = lv_obj_create(card);
    lv_obj_remove_style_all(a->plot);
    lv_obj_set_size(a->plot, a->side, a->side);
    lv_obj_align(a->plot, LV_ALIGN_TOP_MID, 0, top);
    lv_obj_clear_flag(a->plot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    a->circle = lv_obj_create(a->plot);
    lv_obj_remove_style_all(a->circle);
    lv_obj_set_style_radius(a->circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(a->circle, 2, 0);
    lv_obj_set_style_border_color(a->circle, COLOR_MUTED, 0);
    lv_obj_set_style_border_opa(a->circle, LV_OPA_COVER, 0);
    lv_obj_add_flag(a->circle, LV_OBJ_FLAG_HIDDEN);

    for (int b = 0; b < TRAIL_BANDS; b++) {
        lv_obj_t *l = lv_line_create(a->plot);
        // A swing lays the same ground again and again: older bands thin and faint so the
        // overlap reads as a haze, the newest bold on top.
        static const lv_opa_t opa[TRAIL_BANDS] = { 40, 55, 75, 100, 150, 255 };
        lv_obj_set_style_line_width(l, b == TRAIL_BANDS - 1 ? 3 : b == TRAIL_BANDS - 2 ? 2 : 1, 0);
        lv_obj_set_style_line_color(l, COLOR_ACCENT, 0);
        lv_obj_set_style_line_opa(l, opa[b], 0);
        lv_obj_set_style_line_rounded(l, true, 0);
        a->trail[b] = l;
    }

    a->anchor_mark = lv_obj_create(a->plot);
    lv_obj_remove_style_all(a->anchor_mark);
    lv_obj_set_size(a->anchor_mark, 10, 10);
    lv_obj_set_style_radius(a->anchor_mark, 2, 0);
    lv_obj_set_style_bg_color(a->anchor_mark, COLOR_TEXT, 0);
    lv_obj_set_style_bg_opa(a->anchor_mark, LV_OPA_COVER, 0);
    lv_obj_add_flag(a->anchor_mark, LV_OBJ_FLAG_HIDDEN);

    a->boat = lv_line_create(a->plot);
    lv_obj_set_style_line_width(a->boat, 3, 0);
    lv_obj_set_style_line_color(a->boat, COLOR_TEXT, 0);
    lv_obj_set_style_line_rounded(a->boat, true, 0);
    lv_obj_add_flag(a->boat, LV_OBJ_FLAG_HIDDEN);

    a->north = lv_label_create(a->plot);
    lv_label_set_text(a->north, "N " LV_SYMBOL_UP);
    lv_obj_set_style_text_font(a->north, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(a->north, COLOR_MUTED, 0);
    lv_obj_align(a->north, LV_ALIGN_TOP_RIGHT, 0, 0);

    // Fill the trail from the server's position history (background fetch), so it survives restarts.
    if (v->cfg->api[0]) history_request(v->cfg->api, v->cfg->pos_path, v->cfg->trail_min);
}

static bool get_position(const char *path, double *lat, double *lon)
{
    store_value_t v;
    if (!path[0] || !store_get(path, &v, NULL) || v.kind != VAL_POSITION) return false;
    *lat = v.lat;
    *lon = v.lon;
    return true;
}

static bool get_number(const char *path, double *out)
{
    store_value_t v;
    if (!path[0] || !store_get(path, &v, NULL) || v.kind != VAL_NUMBER) return false;
    *out = v.number;
    return true;
}

static void anchor_update(widget_view_t *v)
{
    anchor_view_t *a = v->priv;
    reading_t r;
    if (!a || !read_binding(v, &r)) return;
    const widget_cfg_t *c = v->cfg;

    double blat, blon, alat, alon, radius = 0, heading = 0;
    int64_t now;
    bool have_clock = store_clock_ms(&now);
    bool have_boat = get_position(c->pos_path, &blat, &blon);
    bool have_anchor = get_position(c->anchor_path, &alat, &alon);
    bool have_radius = get_number(c->radius_path, &radius) && radius > 0;
    bool have_heading = get_number(c->heading_path, &heading);
    // Once the server's history arrives, it goes in front of the live points.
    int hn;
    const track_pt_t *hist = history_peek(&hn);
    if (hist) {
        track_t *scratch = heap_caps_malloc(sizeof(track_t), MALLOC_CAP_SPIRAM);
        if (scratch) {
            track_backfill(&a->track, hist, hn, scratch);
            free(scratch);
            a->drawn_us = 0;   // redraw now
        }
        history_release();
    }
    if (have_boat && have_clock) track_add(&a->track, blat, blon, now);   // every update, whatever's on show

    int64_t t_us = esp_timer_get_time();
    if (a->drawn_us && t_us - a->drawn_us < ANCHOR_REDRAW_MS * 1000LL) return;
    a->drawn_us = t_us;

    // Header: distance now / circle radius, in the binding's unit.
    store_value_t dv;
    store_meta_t meta;
    store_get(c->value.path, &dv, &meta);
    char head[72], rad[24];
    if (have_anchor && have_radius) {
        format_number(&c->value, &meta, radius, rad, sizeof(rad));
        snprintf(head, sizeof(head), "%s / %s", r.text, rad);
    } else {
        snprintf(head, sizeof(head), "%s", have_anchor ? r.text : "");
    }
    ui_set_text(v->value, head);
    ui_set_text_color(v->value, r.color);

    // Alarm: a notification under the prefix, or the boat outside the circle.
    alert_t al;
    bool alarm = c->notify_prefix[0] && alerts_worst(c->notify_prefix, &al) > 0;
    bool outside = have_anchor && have_radius && r.is_number && r.si > radius;
    lv_color_t hot = alarm || outside ? COLOR_ALARM : COLOR_TEXT;

    if (!have_boat) {
        lv_obj_add_flag(a->boat, LV_OBJ_FLAG_HIDDEN);
        ui_set_text(a->footer, "Waiting for a position");
        ui_set_text_color(a->footer, COLOR_MUTED);
        return;
    }
    if (alarm) {
        ui_set_text(a->footer, al.message[0] ? al.message : al.path);
        ui_set_text_color(a->footer, COLOR_ALARM);
    } else if (!have_anchor) {
        ui_set_text(a->footer, "Anchor not set: the boat's recent track");
        ui_set_text_color(a->footer, COLOR_MUTED);
    } else {
        ui_set_text(a->footer, outside ? "Outside the circle" : "");
        ui_set_text_color(a->footer, COLOR_ALARM);
    }

    // Scale: centred on the anchor (else the boat), wide enough for the circle and the boat.
    double clat = have_anchor ? alat : blat, clon = have_anchor ? alon : blon;
    double be, bn;
    geo_offset_m(clat, clon, blat, blon, &be, &bn);
    double view = VIEW_MIN_M, d = sqrt(be * be + bn * bn);
    if (have_anchor && have_radius && radius > view) view = radius;
    if (d > view) view = d;
    view *= VIEW_MARGIN;
    double px = (a->side / 2.0 - 6) / view;   // pixels per metre
    lv_coord_t mid = a->side / 2;
#define PX(e) ((lv_coord_t)(mid + (e) * px))
#define PY(n) ((lv_coord_t)(mid - (n) * px))

    if (have_anchor && have_radius) {
        lv_coord_t rp = (lv_coord_t)(radius * px);
        lv_obj_set_size(a->circle, 2 * rp, 2 * rp);
        lv_obj_set_pos(a->circle, mid - rp, mid - rp);
        lv_obj_set_style_border_color(a->circle, alarm || outside ? COLOR_ALARM : COLOR_MUTED, 0);
        lv_obj_clear_flag(a->circle, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(a->circle, LV_OBJ_FLAG_HIDDEN);
    }
    if (have_anchor) {
        lv_obj_set_pos(a->anchor_mark, mid - 5, mid - 5);
        lv_obj_clear_flag(a->anchor_mark, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(a->anchor_mark, LV_OBJ_FLAG_HIDDEN);
    }

    // Trail: the last trail_min minutes, thinned to fit, points off the plot skipped, ending at
    // the boat. Drawn as TRAIL_BANDS lines by age (equal stretches of time), faint to bright, so
    // how she's been lying reads at a glance without using the alarm colours.
    int64_t since = now - (int64_t)c->trail_min * 60000;
    int first = 0;
    while (first < a->track.count && track_at(&a->track, first)->ms < since) first++;
    int span = a->track.count - first, step = span / (TRAIL_DRAW_MAX - 1) + 1, n = 0;
    for (int i = first; i < a->track.count && n < TRAIL_DRAW_MAX - 1; i += step) {
        const track_pt_t *p = track_at(&a->track, i);
        double e, nn;
        geo_offset_m(clat, clon, p->lat_e7 / 1e7, p->lon_e7 / 1e7, &e, &nn);
        if (fabs(e) > view || fabs(nn) > view) continue;
        int band = (int)((p->ms - since) * TRAIL_BANDS / ((int64_t)c->trail_min * 60000 + 1));
        a->trail_band[n] = (uint8_t)(band < 0 ? 0 : band >= TRAIL_BANDS ? TRAIL_BANDS - 1 : band);
        a->trail_pts[n++] = (lv_point_t){ PX(e), PY(nn) };
    }
    a->trail_band[n] = TRAIL_BANDS - 1;
    a->trail_pts[n++] = (lv_point_t){ PX(be), PY(bn) };   // the boat now
    // Points are in time order, so each band is one run; it also takes the next band's first
    // point so the line has no gaps.
    int start = 0;
    for (int b = 0; b < TRAIL_BANDS; b++) {
        int end = start;
        while (end < n && a->trail_band[end] == b) end++;
        int count = end - start + (end < n ? 1 : 0);
        lv_line_set_points(a->trail[b], &a->trail_pts[start], count >= 2 ? count : 0);
        start = end;
    }

    // Boat: a triangle pointing along the heading, or a short bar when there's none.
    lv_coord_t x = PX(be), y = PY(bn);
    double s = sin(heading), co = cos(heading);
    if (have_heading) {
        // tip 14 px ahead; rear corners 9 px back and 7 px to each side
        a->boat_pts[0] = (lv_point_t){ x + (lv_coord_t)(14 * s), y - (lv_coord_t)(14 * co) };
        a->boat_pts[1] = (lv_point_t){ x - (lv_coord_t)(9 * s) + (lv_coord_t)(7 * co), y + (lv_coord_t)(9 * co) + (lv_coord_t)(7 * s) };
        a->boat_pts[2] = (lv_point_t){ x - (lv_coord_t)(9 * s) - (lv_coord_t)(7 * co), y + (lv_coord_t)(9 * co) - (lv_coord_t)(7 * s) };
        a->boat_pts[3] = a->boat_pts[0];
        lv_line_set_points(a->boat, a->boat_pts, 4);
    } else {
        a->boat_pts[0] = (lv_point_t){ x - 6, y };
        a->boat_pts[1] = (lv_point_t){ x + 6, y };
        lv_line_set_points(a->boat, a->boat_pts, 2);
    }
    lv_obj_set_style_line_color(a->boat, hot, 0);
    lv_obj_clear_flag(a->boat, LV_OBJ_FLAG_HIDDEN);
#undef PX
#undef PY
}

static const widget_ops_t s_ops[] = {
    { "value", value_create, value_update },
    { "ring", ring_create, ring_update },
    { "bar", bar_create, bar_update },
    { "state", state_create, state_update },
    { "alert", alert_create, alert_update },
    { "tide", tide_create, tide_update },
    { "anchor", anchor_create, anchor_update },
    { "label", label_create, NULL },
};

static const widget_ops_t *ops_for(int type)
{
    if (type < 0 || type >= widget_type_count) return NULL;
    for (size_t i = 0; i < sizeof(s_ops) / sizeof(s_ops[0]); i++) {
        if (strcmp(s_ops[i].name, widget_types[type].name) == 0) return &s_ops[i];
    }
    return NULL;
}

bool widgets_check_registry(void)
{
    bool ok = true;
    for (int i = 0; i < widget_type_count; i++) {
        if (!ops_for(i)) {
            ESP_LOGE(TAG, "widget type '%s' has no drawing code", widget_types[i].name);
            ok = false;
        }
    }
    return ok;
}

void widget_create(widget_view_t *v, lv_obj_t *card)
{
    const widget_ops_t *ops = ops_for(v->cfg->type);
    v->card = card;
    if (ops && ops->create) ops->create(v, card);
}

void widget_update(widget_view_t *v)
{
    const widget_ops_t *ops = ops_for(v->cfg->type);
    if (ops && ops->update) ops->update(v);
}

void widget_free(widget_view_t *v)
{
    free(v->priv);
    v->priv = NULL;
}
