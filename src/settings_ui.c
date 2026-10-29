#include "settings_ui.h"

#include <stdio.h>
#include <string.h>
#include "board.h"
#include "config_store.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "settings.h"
#include "sk_client.h"
#include "theme.h"
#include "widgets.h"
#include "wifi.h"

#define PAD     24
#define QR_SIZE 180

static lv_obj_t *s_panel;            // Settings, NULL when closed
static lv_obj_t *s_keys, *s_vals;
static lv_obj_t *s_show_btn, *s_wifi_btn;
static bool s_show_pw;
static bool s_starting;              // Change WiFi tapped, the hotspot not on yet
static lv_obj_t *s_setup;            // setup screen, NULL when the hotspot is off
static lv_obj_t *s_phones;

static lv_obj_t *full_screen(void)
{
    lv_obj_t *o = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, BOARD_H_RES, BOARD_V_RES);
    lv_obj_set_style_bg_color(o, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);   // touches stop here, not on the screens below
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, lv_coord_t x, lv_coord_t y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_pos(l, x, y);
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *button(lv_obj_t *parent, const char *label, lv_coord_t x, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, 172, 60);
    lv_obj_set_pos(b, x, BOARD_V_RES - 60 - PAD);
    lv_obj_set_style_bg_color(b, COLOR_CARD, 0);
    lv_obj_set_style_border_color(b, COLOR_TRACK, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(l, COLOR_TEXT, 0);
    lv_label_set_text(l, label);
    lv_obj_center(l);
    return b;
}

static void set_button_text(lv_obj_t *b, const char *t) { ui_set_text(lv_obj_get_child(b, 0), t); }

// ---- Settings panel ----

static void close_panel(void)
{
    if (s_panel) lv_obj_del(s_panel);
    s_panel = NULL;
    s_show_pw = false;
    s_starting = false;
}

static void on_close(lv_event_t *e) { close_panel(); }

static void on_show(lv_event_t *e)
{
    s_show_pw = !s_show_pw;
    set_button_text(s_show_btn, s_show_pw ? LV_SYMBOL_EYE_CLOSE "  Hide" : LV_SYMBOL_EYE_OPEN "  Show");
}

static void on_wifi(lv_event_t *e)
{
    s_starting = true;
    set_button_text(s_wifi_btn, "Starting...");
    wifi_setup_start();   // the setup screen replaces this panel when the hotspot is on
}

static void on_restart(lv_event_t *e) { esp_restart(); }

static void refresh_panel(void)
{
    settings_t set;
    settings_get(&set);
    char vals[512], server[80], line[128];
    int rssi, channel;
    vals[0] = '\0';
    if (wifi_connected() && wifi_link(&rssi, &channel)) {
        snprintf(line, sizeof(line), "%s   %d dBm, channel %d\nhttp://%s/\n", set.wifi_ssid, rssi, channel, wifi_ip());
    } else {
        snprintf(line, sizeof(line), "%s   not joined\n-\n", set.wifi_ssid[0] ? set.wifi_ssid : "none saved");
    }
    strcat(vals, line);
    sk_client_server(server, sizeof(server));
    snprintf(line, sizeof(line), "%s   %s\n", server, sk_client_connected() ? "connected" : "not connected");
    strcat(vals, line);
    snprintf(line, sizeof(line), "%s\n%s\n", esp_app_get_description()->version,
             config_source_name(config_store_running_source()));
    strcat(vals, line);
    snprintf(line, sizeof(line), "%u KB internal, %.1f MB PSRAM\n",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / (1024.0 * 1024.0));
    strcat(vals, line);
    strcat(vals, s_show_pw ? set.password : "- - - - - - - -");
    ui_set_text(s_vals, vals);
}

static void open_panel(lv_event_t *e)
{
    if (s_panel || s_setup) return;
    s_panel = full_screen();
    lv_label_set_text(text(s_panel, &lv_font_montserrat_32, COLOR_TEXT, PAD, PAD - 4), "Settings");
    s_keys = text(s_panel, &lv_font_montserrat_24, COLOR_MUTED, PAD, 80);
    lv_label_set_text(s_keys, "WiFi\nAddress\nSignal K\nFirmware\nConfig\nFree memory\nDevice password");
    s_vals = text(s_panel, &lv_font_montserrat_24, COLOR_TEXT, 260, 80);
    lv_obj_set_width(s_vals, BOARD_H_RES - 260 - PAD);
    lv_label_set_long_mode(s_vals, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_line_space(s_vals, lv_obj_get_style_text_line_space(s_keys, 0), 0);
    s_show_btn = button(s_panel, LV_SYMBOL_EYE_OPEN "  Show", PAD, on_show);
    s_wifi_btn = button(s_panel, "Change WiFi", PAD + 188, on_wifi);
    button(s_panel, "Restart", PAD + 2 * 188, on_restart);
    button(s_panel, "Close", PAD + 3 * 188, on_close);
    refresh_panel();
}

lv_obj_t *settings_ui_add_gear(lv_obj_t *parent)
{
    lv_obj_t *g = lv_label_create(parent);
    lv_label_set_text(g, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_font(g, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(g, COLOR_MUTED, 0);
    lv_obj_add_flag(g, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(g, 24);   // a fingertip, not a stylus
    lv_obj_align(g, LV_ALIGN_TOP_RIGHT, -12, 6);
    lv_obj_add_event_cb(g, open_panel, LV_EVENT_CLICKED, NULL);
    return g;
}

// ---- setup screen (hotspot on) ----

static void on_cancel(lv_event_t *e) { wifi_setup_stop(); }

static void qr(lv_obj_t *parent, lv_coord_t x, const char *data)
{
    lv_obj_t *q = lv_qrcode_create(parent, QR_SIZE, lv_color_black(), lv_color_white());
    lv_qrcode_update(q, data, strlen(data));
    lv_obj_set_style_border_color(q, lv_color_white(), 0);   // the quiet zone a scanner needs
    lv_obj_set_style_border_width(q, 10, 0);
    lv_obj_set_pos(q, x, 112);
}

static void open_setup(const wifi_setup_t *ws)
{
    close_panel();
    s_setup = full_screen();
    lv_label_set_text(text(s_setup, &lv_font_montserrat_32, COLOR_TEXT, PAD, PAD - 4), "Set up WiFi");
    lv_obj_t *why = text(s_setup, &lv_font_montserrat_16, COLOR_MUTED, PAD, 66);
    if (ws->have_saved) {
        lv_label_set_text_fmt(why, "Saved network: %s, not joined now. It's tried again after %d minutes with no phone "
                                   "on this hotspot, or on Cancel.", ws->saved, SETUP_IDLE_S / 60);
    } else {
        lv_label_set_text(why, "No WiFi network saved yet. Use a phone to choose one.");
    }
    lv_obj_set_width(why, BOARD_H_RES - 2 * PAD);
    lv_label_set_long_mode(why, LV_LABEL_LONG_WRAP);

    char join[96];
    snprintf(join, sizeof(join), "WIFI:T:WPA;S:%s;P:%s;;", ws->ssid, ws->pass);
    qr(s_setup, 60, join);
    qr(s_setup, 440, "http://" SETUP_IP "/");
    lv_obj_t *one = text(s_setup, &lv_font_montserrat_24, COLOR_TEXT, 40, 322);
    lv_label_set_text_fmt(one, "1. Join WiFi  %s\n    password  %s", ws->ssid, ws->pass);
    lv_obj_t *two = text(s_setup, &lv_font_montserrat_24, COLOR_TEXT, 420, 322);
    lv_label_set_text(two, "2. Open  http://" SETUP_IP "\n    (most phones open it)");
    s_phones = text(s_setup, &lv_font_montserrat_16, COLOR_MUTED, PAD, BOARD_V_RES - PAD - 40);
    if (ws->have_saved) button(s_setup, "Cancel", BOARD_H_RES - PAD - 172, on_cancel);
}

void settings_ui_refresh(void)
{
    wifi_setup_t ws;
    wifi_setup_info(&ws);
    if (ws.on && !s_setup) open_setup(&ws);
    if (!ws.on && s_setup) {
        lv_obj_del(s_setup);
        s_setup = NULL;
    }
    if (s_setup) {
        ui_set_text(s_phones, ws.phones ? (ws.phones == 1 ? "A phone has joined." : "Phones have joined.")
                                        : "Waiting for a phone to join...");
    }
    if (s_panel) refresh_panel();
}

lv_obj_t *settings_ui_overlay(void) { return s_setup ? s_setup : s_panel; }
