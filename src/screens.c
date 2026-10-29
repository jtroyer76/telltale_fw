#include "screens.h"

#include <stdio.h>
#include <string.h>
#include "alerts.h"
#include "board.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "ota.h"
#include "settings_ui.h"
#include "sk_client.h"
#include "store.h"
#include "tides.h"
#include "theme.h"
#include "widget_types.h"
#include "widgets.h"
#include "wifi.h"

#define HEADER_H        36
#define GAP             8
#define REFRESH_MS      500
#define QUIET_S         30      // connected but nothing received for this long: "no data"

static const char *TAG = "screens";

static widget_view_t s_views[CFG_MAX_SCREENS * CFG_MAX_WIDGETS];
static int s_view_count;
static lv_obj_t *s_tiles[CFG_MAX_SCREENS];
static int s_tile_count;
static lv_obj_t *s_tileview;
static lv_obj_t *s_status;
static lv_obj_t *s_notice;
static lv_timer_t *s_timer;
static char s_notice_text[192];

// auto_screen: last value acted on, and when the user last swiped.
static const auto_screen_t *s_auto;
static char s_auto_last[STORE_TEXT_LEN];
static int64_t s_manual_us;
static bool s_switching;   // a switch we made, not a swipe

static lv_obj_t *make_card(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return card;
}

static void build_widget(lv_obj_t *tile, int screen, const screen_cfg_t *s, const widget_cfg_t *w)
{
    lv_coord_t area_w = BOARD_H_RES - GAP;
    lv_coord_t area_h = BOARD_V_RES - HEADER_H - GAP;
    lv_coord_t cell_w = area_w / s->cols;
    lv_coord_t cell_h = area_h / s->rows;
    lv_obj_t *card = make_card(tile, GAP + w->col * cell_w, HEADER_H + GAP + w->row * cell_h,
                               w->w * cell_w - GAP, w->h * cell_h - GAP);

    widget_view_t *v = &s_views[s_view_count++];
    *v = (widget_view_t){ .cfg = w };
    widget_create(v, card);
    const widget_type_t *t = &widget_types[w->type];
    // An anchor widget's prefix is optional ("" = none); an alert widget's "" means every notification.
    if (t->takes_notifications && (!t->takes_anchor || w->notify_prefix[0])) alerts_watch(w->notify_prefix, screen);
    // The anchor view's swing trail needs positions whatever screen is on show.
    int when = t->takes_anchor ? STORE_BACKGROUND : screen;
    if (t->takes_value && w->value.path[0]) store_watch(w->value.path, when);
    if (t->takes_anchor) {
        store_watch(w->pos_path, STORE_BACKGROUND);
        store_watch(w->anchor_path, STORE_BACKGROUND);
        store_watch(w->radius_path, STORE_BACKGROUND);
        if (w->heading_path[0]) store_watch(w->heading_path, STORE_BACKGROUND);
    }
}

static void refresh_status(void)
{
    char server[80], text[160];
    sk_client_server(server, sizeof(server));
    if (ota_progress() >= 0) {
        snprintf(text, sizeof(text), "Updating firmware  %d%%", ota_progress());
        ui_set_text_color(s_status, COLOR_WARN);
    } else if (!wifi_connected()) {
        snprintf(text, sizeof(text), "WiFi: connecting");
        ui_set_text_color(s_status, COLOR_WARN);
    } else if (!sk_client_connected()) {
        bool searching = strncmp(server, "searching", 9) == 0;
        snprintf(text, sizeof(text), "%s  |  Signal K: %s%s", wifi_ip(), searching ? "" : "connecting to ", server);
        ui_set_text_color(s_status, COLOR_WARN);
    } else if (esp_timer_get_time() - sk_client_last_message_us() > (int64_t)QUIET_S * 1000000) {
        snprintf(text, sizeof(text), "%s  |  Signal K: no data", wifi_ip());
        ui_set_text_color(s_status, COLOR_WARN);
    } else {
        snprintf(text, sizeof(text), "%s  |  Signal K: live", wifi_ip());
        ui_set_text_color(s_status, COLOR_OK);
    }
    ui_set_text(s_status, text);
}

static void set_active(int index);

// Follows the auto_screen value (e.g. navigation.state). Acts on changes only, so a swipe
// sticks until the state changes again and the manual hold has run out.
static void refresh_auto(void)
{
    if (!s_auto || !s_auto->path[0]) return;
    store_value_t v;
    if (!store_get(s_auto->path, &v, NULL) || v.kind != VAL_TEXT || strcmp(v.text, s_auto_last) == 0) return;
    if (s_manual_us && esp_timer_get_time() - s_manual_us < (int64_t)s_auto->manual_hold_s * 1000000) return;

    snprintf(s_auto_last, sizeof(s_auto_last), "%s", v.text);
    for (int i = 0; i < s_auto->count; i++) {
        if (strcmp(s_auto->states[i], v.text) == 0 && s_auto->screens[i] != store_active_screen()) {
            ESP_LOGI(TAG, "%s is '%s': showing screen %d", s_auto->path, v.text, s_auto->screens[i]);
            s_switching = true;
            lv_obj_set_tile_id(s_tileview, s_auto->screens[i], 0, LV_ANIM_ON);
            set_active(s_auto->screens[i]);
            s_switching = false;
        }
    }
}

static void refresh(lv_timer_t *t)
{
    for (int i = 0; i < s_view_count; i++) widget_update(&s_views[i]);
    refresh_auto();
    refresh_status();
    settings_ui_refresh();
}

static void build_header(lv_obj_t *tile, const char *title)
{
    lv_obj_t *t = lv_label_create(tile);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(t, COLOR_TEXT, 0);
    lv_obj_set_pos(t, GAP + 4, 6);
}

static void set_active(int index)
{
    if (index == store_active_screen()) return;
    store_set_active_screen(index);
    sk_client_resubscribe();
}

static void on_tile_changed(lv_event_t *e)
{
    lv_obj_t *act = lv_tileview_get_tile_act(s_tileview);
    for (int i = 0; i < s_tile_count; i++) {
        if (s_tiles[i] != act || i == store_active_screen()) continue;
        if (!s_switching) s_manual_us = esp_timer_get_time();
        set_active(i);
    }
}

// Status and notice sit on the screen, above the tiles, so they show on every page.
static void build_overlay(lv_obj_t *scr)
{
    settings_ui_add_gear(scr);
    s_status = lv_label_create(scr);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_16, 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, -GAP - 4 - 40, 10);   // left of the gear

    s_notice = lv_label_create(scr);
    lv_label_set_long_mode(s_notice, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_notice, BOARD_H_RES - 2 * GAP);
    lv_obj_set_style_text_font(s_notice, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_notice, lv_color_black(), 0);
    lv_obj_set_style_bg_color(s_notice, COLOR_WARN, 0);
    lv_obj_set_style_bg_opa(s_notice, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_notice, 4, 0);
    lv_obj_align(s_notice, LV_ALIGN_BOTTOM_MID, 0, -2);
    screens_set_notice(s_notice_text);

    if (s_timer) lv_timer_del(s_timer);
    s_timer = lv_timer_create(refresh, REFRESH_MS, NULL);
}

static void reset_screen(lv_obj_t *scr)
{
    for (int i = 0; i < s_view_count; i++) widget_free(&s_views[i]);
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, COLOR_BG, 0);
    s_view_count = 0;
    s_tile_count = 0;
    s_tileview = NULL;
    s_auto = NULL;
    s_auto_last[0] = '\0';
    s_manual_us = 0;
}

void screens_build(const config_t *cfg)
{
    lv_obj_t *scr = lv_scr_act();
    reset_screen(scr);
    store_set_active_screen(0);

    s_tileview = lv_tileview_create(scr);
    lv_obj_set_style_bg_color(s_tileview, COLOR_BG, 0);
    lv_obj_set_scrollbar_mode(s_tileview, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_tileview, on_tile_changed, LV_EVENT_VALUE_CHANGED, NULL);

    // Local time for the widgets that show clock times; the offset is needed on every screen.
    widgets_set_clock(cfg->clock_offset_path, cfg->clock_hour12);
    if (cfg->clock_offset_path[0]) store_watch(cfg->clock_offset_path, STORE_BACKGROUND);

    for (int i = 0; i < cfg->screen_count; i++) {
        const screen_cfg_t *s = &cfg->screens[i];
        lv_dir_t dir = (i > 0 ? LV_DIR_LEFT : 0) | (i < cfg->screen_count - 1 ? LV_DIR_RIGHT : 0);
        lv_obj_t *tile = lv_tileview_add_tile(s_tileview, i, 0, dir);
        s_tiles[s_tile_count++] = tile;
        build_header(tile, s->title);
        for (int j = 0; j < s->widget_count; j++) build_widget(tile, i, s, &s->widgets[j]);
    }

    s_auto = &cfg->auto_screen;
    if (s_auto->path[0]) store_watch(s_auto->path, STORE_BACKGROUND);

    // Tide predictions: one source (the first tide widget's api), as far ahead as any widget shows.
    const char *tide_api = NULL;
    int tide_hours = 0;
    for (int i = 0; i < cfg->screen_count; i++) {
        for (int j = 0; j < cfg->screens[i].widget_count; j++) {
            const widget_cfg_t *w = &cfg->screens[i].widgets[j];
            if (!widget_types[w->type].takes_tide) continue;
            if (!tide_api) tide_api = w->api;
            if (w->hours > tide_hours) tide_hours = w->hours;
        }
    }
    tides_configure(tide_api, tide_hours);

    build_overlay(scr);
    refresh(NULL);
}

void screens_show_error(const char *msg)
{
    lv_obj_t *scr = lv_scr_act();
    reset_screen(scr);
    lv_obj_t *l = lv_label_create(scr);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, BOARD_H_RES - 80);
    lv_label_set_text_fmt(l, "Config error\n\n%s\n\nSend a config to http://<address above>/api/config", msg);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(l, COLOR_WARN, 0);
    lv_obj_center(l);
    build_overlay(scr);
    refresh(NULL);
}

void screens_set_notice(const char *msg)
{
    if (msg != s_notice_text) snprintf(s_notice_text, sizeof(s_notice_text), "%s", msg ? msg : "");
    if (!s_notice) return;
    lv_label_set_text(s_notice, s_notice_text);
    if (s_notice_text[0]) {
        lv_obj_clear_flag(s_notice, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_notice, LV_OBJ_FLAG_HIDDEN);
    }
}

bool screens_show(int index)
{
    if (!s_tileview || index < 0 || index >= s_tile_count) return false;
    lv_obj_set_tile_id(s_tileview, index, 0, LV_ANIM_ON);
    s_manual_us = esp_timer_get_time();   // same as a swipe
    set_active(index);
    return true;
}

int screens_active(void) { return store_active_screen(); }
