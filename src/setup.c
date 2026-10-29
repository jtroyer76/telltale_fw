#include "setup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "auth.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "settings.h"
#include "wifi.h"

#define MAX_FORM  2048
#define PAGE_LEN  8192

static const char *TAG = "setup";
static char s_hostname[32];

static const char PAGE_HEAD[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'><title>Telltale setup</title><style>"
    "body{font:17px system-ui,sans-serif;background:#0b1220;color:#e8edf5;margin:0;padding:16px;max-width:520px}"
    "h1{font-size:24px;margin:4px 0 12px}h2{font-size:18px;margin:22px 0 6px;color:#8a96ab}"
    "label{display:block;margin:10px 0 4px}p{color:#8a96ab;margin:4px 0}"
    "input{box-sizing:border-box;width:100%;font:inherit;padding:10px;border-radius:8px;border:1px solid #34466e;"
    "background:#16213a;color:#e8edf5}"
    ".nets button{font:inherit;margin:4px 6px 0 0;padding:6px 10px;border-radius:8px;border:1px solid #34466e;"
    "background:#16213a;color:#e8edf5}"
    ".save{display:block;width:100%;margin-top:24px;padding:12px;font:inherit;font-weight:600;border:0;"
    "border-radius:8px;background:#4ea1ff;color:#0b1220}.err{color:#ff5a5a}"
    "</style></head><body>";

// Appends `s` to `out` with the characters HTML treats specially escaped.
static void put_escaped(char *out, size_t max, const char *s)
{
    size_t n = strlen(out);
    for (; *s && n + 7 < max; s++) {
        const char *e = *s == '&' ? "&amp;" : *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : *s == '"' ? "&quot;"
                      : *s == '\'' ? "&#39;" : NULL;
        if (e) {
            strcpy(out + n, e);
            n += strlen(e);
        } else {
            out[n++] = *s;
        }
    }
    out[n] = '\0';
}

static void put(char *out, size_t max, const char *s)
{
    size_t n = strlen(out);
    snprintf(out + n, max - n, "%s", s);
}

static esp_err_t send_page(httpd_req_t *req, const char *error)
{
    settings_t set;
    settings_get(&set);
    wifi_setup_t ws;
    wifi_setup_info(&ws);
    char (*nets)[33] = heap_caps_malloc(sizeof(char[33]) * SETUP_MAX_NETS, MALLOC_CAP_SPIRAM);
    char *page = heap_caps_malloc(PAGE_LEN, MALLOC_CAP_SPIRAM);
    if (!nets || !page) {
        free(nets);
        free(page);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    page[0] = '\0';
    put(page, PAGE_LEN, PAGE_HEAD);
    put(page, PAGE_LEN, "<h1>Telltale setup</h1>");
    if (error) {
        put(page, PAGE_LEN, "<p class=err>");
        put_escaped(page, PAGE_LEN, error);
        put(page, PAGE_LEN, "</p>");
    }
    put(page, PAGE_LEN, "<form method=post action=/setup id=f><h2>WiFi network</h2><label>Network name</label>"
                        "<input name=ssid required maxlength=32 autocapitalize=off autocorrect=off value=\"");
    put_escaped(page, PAGE_LEN, set.wifi_ssid);
    put(page, PAGE_LEN, "\">");
    int n = wifi_setup_networks(nets, SETUP_MAX_NETS);
    if (n) {
        put(page, PAGE_LEN, "<p>In range (tap one):</p><div class=nets>");
        for (int i = 0; i < n; i++) {
            put(page, PAGE_LEN, "<button type=button onclick='f.ssid.value=this.textContent'>");
            put_escaped(page, PAGE_LEN, nets[i]);
            put(page, PAGE_LEN, "</button>");
        }
        put(page, PAGE_LEN, "</div>");
    }
    put(page, PAGE_LEN, "<label>Password</label><input name=wpass type=password maxlength=63 placeholder=\"");
    put(page, PAGE_LEN, set.wifi_ssid[0] ? "Blank: keep the saved one" : "Blank: an open network");
    put(page, PAGE_LEN, "\">"
                        "<h2>Signal K server</h2><label>Address</label>"
                        "<input name=sk autocapitalize=off autocorrect=off placeholder=\"Blank: find it automatically\" value=\"");
    if (set.sk_host[0]) {
        char sk[SETTINGS_HOST_LEN + 8];
        snprintf(sk, sizeof(sk), set.sk_port == 80 ? "%s" : "%s:%d", set.sk_host, set.sk_port);
        put_escaped(page, PAGE_LEN, sk);
    }
    put(page, PAGE_LEN, "\"><p>For example 192.168.1.50 or signalk.local:3000. Blank uses the config's server, "
                        "or finds one on the network.</p>"
                        "<h2>Device password</h2><p>Needed to update the firmware or change the config from a computer. "
                        "It's shown on the display under Settings.</p><label>New device password</label>"
                        "<input name=newpw maxlength=32 autocapitalize=off autocorrect=off placeholder=\"Blank: keep it\">");
    if (!ws.on) {   // from the boat network: prove it's you
        put(page, PAGE_LEN, "<label>Current device password</label><input name=curpw type=password required>");
    }
    put(page, PAGE_LEN, "<button class=save>Save and restart</button></form></body></html>");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, page);
    free(page);
    free(nets);
    return err;
}

static esp_err_t page_get(httpd_req_t *req) { return send_page(req, NULL); }

// One form field, URL-decoded ("" if absent). False if it doesn't fit.
static bool form_field(const char *form, const char *key, char *out, size_t len)
{
    char raw[3 * 64 + 1];
    out[0] = '\0';
    esp_err_t err = httpd_query_key_value(form, key, raw, sizeof(raw));
    if (err == ESP_ERR_NOT_FOUND) return true;
    if (err != ESP_OK) return false;
    size_t n = 0;
    for (const char *p = raw; *p; p++) {
        if (n + 1 >= len) return false;
        if (*p == '+') {
            out[n++] = ' ';
        } else if (*p == '%' && p[1] && p[2]) {
            char hex[3] = { p[1], p[2], 0 };
            out[n++] = (char)strtol(hex, NULL, 16);
            p += 2;
        } else {
            out[n++] = *p;
        }
    }
    out[n] = '\0';
    return true;
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));   // let the reply go out
    esp_restart();
}

static esp_err_t setup_post(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len >= MAX_FORM) return send_page(req, "Form too large");
    char *form = malloc(req->content_len + 1);
    if (!form) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, form + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            free(form);
            return ESP_FAIL;
        }
        got += r;
    }
    form[got] = '\0';

    settings_t old, s;
    settings_get(&old);
    s = old;
    char wpass[SETTINGS_WIFI_PW_LEN], sk[SETTINGS_HOST_LEN + 8], newpw[SETTINGS_PW_LEN], curpw[SETTINGS_PW_LEN];
    bool fits = form_field(form, "ssid", s.wifi_ssid, sizeof(s.wifi_ssid)) &&
                form_field(form, "wpass", wpass, sizeof(wpass)) &&
                form_field(form, "sk", sk, sizeof(sk)) &&
                form_field(form, "newpw", newpw, sizeof(newpw)) &&
                form_field(form, "curpw", curpw, sizeof(curpw));
    free(form);
    if (!fits) return send_page(req, "Something in the form is too long");

    wifi_setup_t ws;
    wifi_setup_info(&ws);
    if (!ws.on && !auth_password_ok(curpw)) return send_page(req, "Wrong device password");

    // A blank WiFi password keeps the saved one, for the same network.
    if (wpass[0] || strcmp(s.wifi_ssid, old.wifi_ssid) != 0) snprintf(s.wifi_pass, sizeof(s.wifi_pass), "%s", wpass);
    if (newpw[0]) snprintf(s.password, sizeof(s.password), "%s", newpw);
    s.sk_port = 80;
    char *colon = strchr(sk, ':');
    if (colon) {
        *colon = '\0';
        s.sk_port = atoi(colon + 1);
    }
    if (strlen(sk) >= sizeof(s.sk_host)) return send_page(req, "Signal K server: the name is too long");
    memcpy(s.sk_host, sk, strlen(sk) + 1);
    if (!s.sk_host[0]) s.sk_port = 80;

    const char *bad = settings_check(&s);
    if (bad) return send_page(req, bad);
    if (!settings_save(&s)) return send_page(req, "Couldn't save the settings");

    char *page = malloc(2048);
    if (!page) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    page[0] = '\0';
    put(page, 2048, PAGE_HEAD);
    put(page, 2048, "<h1>Saved</h1><p>The display is restarting and will join <b>");
    put_escaped(page, 2048, s.wifi_ssid);
    put(page, 2048, "</b>.</p><p>Put your phone back on that network. The display's page will be at <b>http://");
    put_escaped(page, 2048, s_hostname);
    put(page, 2048, ".local/</b>, and its address is on the screen under Settings.</p></body></html>");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, page);
    free(page);
    ESP_LOGI(TAG, "settings saved, restarting");
    xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// The phone's "is there internet?" check, or a mistyped address: on the hotspot, go to the page.
static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t code)
{
    wifi_setup_t ws;
    wifi_setup_info(&ws);
    if (!ws.on) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" SETUP_IP "/");
    return httpd_resp_send(req, NULL, 0);
}

void setup_register(httpd_handle_t server, const char *hostname)
{
    snprintf(s_hostname, sizeof(s_hostname), "%s", hostname);
    static const httpd_uri_t page = { .uri = "/", .method = HTTP_GET, .handler = page_get };
    static const httpd_uri_t save = { .uri = "/setup", .method = HTTP_POST, .handler = setup_post };
    httpd_register_uri_handler(server, &page);
    httpd_register_uri_handler(server, &save);
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found);
}
