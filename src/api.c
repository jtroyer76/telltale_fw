#include "api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "alerts.h"
#include "app.h"
#include "auth.h"
#include "cJSON.h"
#include "config_store.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fs.h"
#include "lvgl_port.h"
#include "screens.h"
#include "settings_ui.h"
#include "sk_client.h"
#include "store.h"
#include "tides.h"
#include "widget_types.h"
#include "wifi.h"

static const char *TAG = "api";

static esp_err_t send_json(httpd_req_t *req, const char *status, cJSON *body)
{
    char *text = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (status) httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, text ? text : "{}");
    cJSON_free(text);
    return err;
}

static esp_err_t send_result(httpd_req_t *req, bool ok, const char *error)
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddBoolToObject(body, "ok", ok);
    if (!ok) cJSON_AddStringToObject(body, "error", error);
    return send_json(req, ok ? NULL : "400 Bad Request", body);
}

// Whole request body, NUL-terminated (caller frees). NULL after sending an error.
static char *read_body(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > CFG_MAX_JSON) {
        send_result(req, false, req->content_len ? "config too large" : "empty body");
        return NULL;
    }
    char *buf = heap_caps_malloc(req->content_len + 1, MALLOC_CAP_SPIRAM);
    if (!buf) {
        send_result(req, false, "out of memory");
        return NULL;
    }
    size_t got = 0;
    int timeouts = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, buf + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) continue;   // don't hold the HTTP task forever
        if (n <= 0) {
            free(buf);
            return NULL;
        }
        got += n;
    }
    buf[got] = '\0';
    return buf;
}

static esp_err_t config_get(httpd_req_t *req)
{
    char *json = config_store_running_json();
    if (!json) return send_result(req, false, "no config running");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "X-Config-Source", config_source_name(config_store_running_source()));
    esp_err_t err = httpd_resp_sendstr(req, json);
    free(json);
    return err;
}

static esp_err_t config_lkg_get(httpd_req_t *req)
{
    char *json = fs_read(CONFIG_LKG_PATH, CFG_MAX_JSON);
    if (!json) {
        cJSON *body = cJSON_CreateObject();
        cJSON_AddBoolToObject(body, "ok", false);
        cJSON_AddStringToObject(body, "error", "no last-known-good config");
        return send_json(req, "404 Not Found", body);
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json);
    free(json);
    return err;
}

static esp_err_t config_validate(httpd_req_t *req)
{
    char *json = read_body(req);
    if (!json) return ESP_OK;
    config_t *cfg = app_new_config();
    char err[160] = "out of memory";
    bool ok = cfg && config_parse(json, cfg, err, sizeof(err));
    free(cfg);
    free(json);
    return send_result(req, ok, err);
}

static esp_err_t config_put(httpd_req_t *req)
{
    if (!auth_check(req)) return ESP_OK;
    char *json = read_body(req);
    if (!json) return ESP_OK;
    config_t *cfg = app_new_config();
    char err[160] = "out of memory";
    bool ok = cfg && config_store_save(json, cfg, err, sizeof(err));
    free(json);
    if (!ok) {
        free(cfg);
        return send_result(req, false, err);
    }
    bool restart = app_apply_config(cfg);
    cJSON *body = cJSON_CreateObject();
    cJSON_AddBoolToObject(body, "ok", true);
    cJSON_AddBoolToObject(body, "restart_required", restart);
    return send_json(req, NULL, body);
}

static esp_err_t config_delete(httpd_req_t *req)
{
    if (!auth_check(req)) return ESP_OK;
    config_store_reset();
    config_t *cfg = app_new_config();
    char note[192];
    if (!cfg || config_store_load(cfg, 2, note, sizeof(note)) != CFG_SRC_BUILTIN) {
        free(cfg);
        return send_result(req, false, "built-in config failed to load");
    }
    app_apply_config(cfg);
    return send_result(req, true, NULL);
}

static esp_err_t widgets_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    for (int i = 0; i < widget_type_count; i++) {
        if (i) httpd_resp_sendstr_chunk(req, ",");
        httpd_resp_sendstr_chunk(req, widget_types[i].descriptor);
    }
    httpd_resp_sendstr_chunk(req, "]");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static void add_path(const char *path, void *ctx) { cJSON_AddItemToArray((cJSON *)ctx, cJSON_CreateString(path)); }

static esp_err_t status_get(httpd_req_t *req)
{
    char server[80];
    sk_client_server(server, sizeof(server));
    int64_t now = esp_timer_get_time();
    int64_t last = sk_client_last_message_us();

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "ip", wifi_ip());
    int rssi, channel;
    if (wifi_link(&rssi, &channel)) {
        cJSON_AddNumberToObject(body, "rssi_dbm", rssi);
        cJSON_AddNumberToObject(body, "channel", channel);
    }
    int reason, drops = wifi_disconnects(&reason);
    cJSON_AddNumberToObject(body, "wifi_drops", drops);
    if (reason) cJSON_AddNumberToObject(body, "wifi_last_reason", reason);
    wifi_setup_t ws;
    wifi_setup_info(&ws);
    cJSON_AddBoolToObject(body, "setup_hotspot", ws.on);
    cJSON_AddNumberToObject(body, "uptime_s", (double)(now / 1000000));
    cJSON_AddNumberToObject(body, "free_internal", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(body, "free_psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddStringToObject(body, "config_source", config_source_name(config_store_running_source()));
    cJSON *sk = cJSON_AddObjectToObject(body, "signalk");
    cJSON_AddStringToObject(sk, "server", server);
    cJSON_AddBoolToObject(sk, "connected", sk_client_connected());
    if (last) cJSON_AddNumberToObject(sk, "last_message_age_s", (double)(now - last) / 1e6);
    cJSON_AddNumberToObject(body, "screen", store_active_screen());
    tides_t *td = heap_caps_malloc(sizeof(tides_t), MALLOC_CAP_SPIRAM);
    if (td) {
        cJSON *tj = cJSON_AddObjectToObject(body, "tides");
        cJSON_AddBoolToObject(tj, "ok", tides_get(td));
        if (td->ok) {
            cJSON_AddStringToObject(tj, "station", td->station);
            cJSON_AddNumberToObject(tj, "points", td->n_pts);
            cJSON_AddNumberToObject(tj, "highs_lows", td->n_ext);
        }
        free(td);
    }
    alert_t worst;
    cJSON *al = cJSON_AddObjectToObject(body, "alerts");
    cJSON_AddBoolToObject(al, "live", alerts_live());
    int active = alerts_worst("", &worst);
    cJSON_AddNumberToObject(al, "active", active);
    if (active) {
        cJSON_AddStringToObject(al, "worst_path", worst.path);
        cJSON_AddStringToObject(al, "worst_state", zone_state_name(worst.state));
        cJSON_AddStringToObject(al, "worst_message", worst.message);
    }

    // Values the visible screen is subscribed to, with age (from the server's timestamps).
    cJSON *names = cJSON_CreateArray();
    store_for_each_subscribed(add_path, names);
    cJSON *values = cJSON_AddObjectToObject(body, "values");
    const cJSON *n;
    cJSON_ArrayForEach(n, names) {
        store_value_t v;
        store_meta_t m;
        if (!store_get(n->valuestring, &v, &m)) continue;
        cJSON *e = cJSON_AddObjectToObject(values, n->valuestring);
        if (v.kind == VAL_NUMBER) cJSON_AddNumberToObject(e, "value", v.number);
        if (v.kind == VAL_TEXT) cJSON_AddStringToObject(e, "value", v.text);
        if (v.kind == VAL_POSITION) {
            cJSON *p = cJSON_AddObjectToObject(e, "value");
            cJSON_AddNumberToObject(p, "latitude", v.lat);
            cJSON_AddNumberToObject(p, "longitude", v.lon);
        }
        if (v.kind != VAL_NONE && v.updated_us) {
            int64_t age = now - v.updated_us;
            cJSON_AddNumberToObject(e, "age_s", age > 0 ? (double)age / 1e6 : 0);
        }
        if (m.has_meta) {
            cJSON_AddStringToObject(e, "si", m.si);
            if (m.unit[0]) cJSON_AddStringToObject(e, "display_unit", m.unit);
            if (m.decimals >= 0) cJSON_AddNumberToObject(e, "display_decimals", m.decimals);
            if (m.zone_count) cJSON_AddNumberToObject(e, "zones", m.zone_count);
        }
    }
    cJSON_Delete(names);
    return send_json(req, NULL, body);
}

static esp_err_t screen_post(httpd_req_t *req)
{
    char query[32], val[8];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "index", val, sizeof(val)) != ESP_OK) {
        return send_result(req, false, "need ?index=N");
    }
    bool ok = false;
    if (lvgl_port_lock(1000)) {
        ok = screens_show(atoi(val));
        lvgl_port_unlock();
    }
    return send_result(req, ok, "no such screen");
}

static void put_le(uint8_t *p, uint32_t v, int n)
{
    for (int i = 0; i < n; i++) p[i] = v >> (8 * i);
}

// The screen as shown, as a 16-bit BMP (RGB565, top row first). With Settings or the setup
// screen open, that's what's captured. For docs: tools/telltale.py screenshot.
static esp_err_t screenshot_get(httpd_req_t *req)
{
    if (!lvgl_port_lock(-1)) return send_result(req, false, "display busy");
    lv_obj_t *obj = settings_ui_overlay();
    if (!obj) obj = lv_scr_act();
    uint32_t size = lv_snapshot_buf_size_needed(obj, LV_IMG_CF_TRUE_COLOR);
    uint8_t *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    lv_img_dsc_t dsc;
    bool ok = buf && lv_snapshot_take_to_buf(obj, LV_IMG_CF_TRUE_COLOR, &dsc, buf, size) == LV_RES_OK;
    lvgl_port_unlock();
    if (!ok) {
        free(buf);
        return send_result(req, false, "snapshot failed");
    }
    uint32_t w = dsc.header.w, h = dsc.header.h, data = w * h * 2;   // rows of 1600 bytes: no padding
    uint8_t hdr[66] = { 'B', 'M' };
    put_le(hdr + 2, sizeof(hdr) + data, 4);
    put_le(hdr + 10, sizeof(hdr), 4);
    put_le(hdr + 14, 40, 4);
    put_le(hdr + 18, w, 4);
    put_le(hdr + 22, (uint32_t)-(int32_t)h, 4);   // negative height: top row first
    put_le(hdr + 26, 1, 2);
    put_le(hdr + 28, 16, 2);
    put_le(hdr + 30, 3, 4);                       // BI_BITFIELDS, masks below
    put_le(hdr + 34, data, 4);
    put_le(hdr + 54, 0xF800, 4);
    put_le(hdr + 58, 0x07E0, 4);
    put_le(hdr + 62, 0x001F, 4);
    httpd_resp_set_type(req, "image/bmp");
    esp_err_t err = httpd_resp_send_chunk(req, (const char *)hdr, sizeof(hdr));
    for (uint32_t off = 0; err == ESP_OK && off < data; off += 16384) {
        err = httpd_resp_send_chunk(req, (const char *)buf + off, data - off < 16384 ? data - off : 16384);
    }
    free(buf);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);
    return err;
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t restart_post(httpd_req_t *req)
{
    if (!auth_check(req)) return ESP_OK;
    ESP_LOGI(TAG, "restart requested");
    send_result(req, true, NULL);
    xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// Turns the setup hotspot on, as Change WiFi on the screen does. The display leaves the boat
// network until it's set up, cancelled on the screen, or SETUP_IDLE_S passes with no phone on it.
static esp_err_t wifi_setup_post(httpd_req_t *req)
{
    if (!auth_check(req)) return ESP_OK;
    ESP_LOGI(TAG, "setup hotspot requested");
    send_result(req, true, NULL);
    wifi_setup_start();
    return ESP_OK;
}

void api_register(httpd_handle_t server)
{
    static const httpd_uri_t uris[] = {
        { .uri = "/api/config", .method = HTTP_GET, .handler = config_get },
        { .uri = "/api/config", .method = HTTP_PUT, .handler = config_put },
        { .uri = "/api/config", .method = HTTP_DELETE, .handler = config_delete },
        { .uri = "/api/config/validate", .method = HTTP_POST, .handler = config_validate },
        { .uri = "/api/config/lkg", .method = HTTP_GET, .handler = config_lkg_get },
        { .uri = "/api/widgets", .method = HTTP_GET, .handler = widgets_get },
        { .uri = "/api/status", .method = HTTP_GET, .handler = status_get },
        { .uri = "/api/screen", .method = HTTP_POST, .handler = screen_post },
        { .uri = "/api/restart", .method = HTTP_POST, .handler = restart_post },
        { .uri = "/api/wifi/setup", .method = HTTP_POST, .handler = wifi_setup_post },
        { .uri = "/api/screenshot", .method = HTTP_GET, .handler = screenshot_get },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(server, &uris[i]);
}
