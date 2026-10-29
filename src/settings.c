#include "settings.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

// Development builds: secrets.h stands in for settings not saved yet (secrets.example.h).
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif
#ifndef SK_HOST_OVERRIDE
#define SK_HOST_OVERRIDE ""
#define SK_PORT_OVERRIDE 80
#endif

#define NS             "telltale"
#define GEN_PW_LEN     12
// No 0/O, 1/l/i: the password is read off the screen and typed.
static const char PW_CHARS[] = "abcdefghjkmnpqrstuvwxyz23456789";

static const char *TAG = "settings";
static settings_t s_set;
static SemaphoreHandle_t s_lock;

static void get_str(nvs_handle_t h, const char *key, char *out, size_t len, const char *fallback)
{
    size_t n = len;
    if (!h || nvs_get_str(h, key, out, &n) != ESP_OK) snprintf(out, len, "%s", fallback);
}

void settings_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    nvs_handle_t h = 0;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) h = 0;
    get_str(h, "wifi_ssid", s_set.wifi_ssid, sizeof(s_set.wifi_ssid), WIFI_SSID);
    get_str(h, "wifi_pass", s_set.wifi_pass, sizeof(s_set.wifi_pass), WIFI_PASS);
    get_str(h, "sk_host", s_set.sk_host, sizeof(s_set.sk_host), SK_HOST_OVERRIDE);
    int32_t port = SK_PORT_OVERRIDE;
    if (h) nvs_get_i32(h, "sk_port", &port);
    s_set.sk_port = port;
#ifdef OTA_PASS
    get_str(h, "password", s_set.password, sizeof(s_set.password), OTA_PASS);
#else
    get_str(h, "password", s_set.password, sizeof(s_set.password), "");
#endif
    if (!s_set.password[0]) {   // first boot: make one up, and keep it
        for (int i = 0; i < GEN_PW_LEN; i++) s_set.password[i] = PW_CHARS[esp_random() % (sizeof(PW_CHARS) - 1)];
        s_set.password[GEN_PW_LEN] = '\0';
        if (h) {
            nvs_set_str(h, "password", s_set.password);
            nvs_commit(h);
        }
        ESP_LOGI(TAG, "device password made up (see Settings on the screen)");
    }
    if (h) nvs_close(h);
    ESP_LOGI(TAG, "WiFi: %s; Signal K: %s", s_set.wifi_ssid[0] ? s_set.wifi_ssid : "(none, setup hotspot)",
             s_set.sk_host[0] ? s_set.sk_host : "from config");
}

void settings_get(settings_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_set;
    xSemaphoreGive(s_lock);
}

const char *settings_check(const settings_t *s)
{
    size_t wp = strlen(s->wifi_pass), pw = strlen(s->password);
    if (!s->wifi_ssid[0]) return "Choose a WiFi network";
    if (wp && (wp < 8 || wp > 63)) return "A WiFi password is 8 to 63 characters";
    if (pw < SETTINGS_PW_MIN || pw >= SETTINGS_PW_LEN) return "The device password must be 8 to 32 characters";
    for (const char *c = s->password; *c; c++) {
        if (*c < '!' || *c > '~') return "The device password: letters, digits and symbols only (no spaces)";
    }
    for (const char *c = s->sk_host; *c; c++) {
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '.' || *c == '-')) {
            return "Signal K server: a name or IP address, e.g. 192.168.1.50 or signalk.local";
        }
    }
    if (s->sk_port < 1 || s->sk_port > 65535) return "Signal K port must be 1 to 65535";
    return NULL;
}

bool settings_save(const settings_t *s)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_str(h, "wifi_ssid", s->wifi_ssid) == ESP_OK &&
              nvs_set_str(h, "wifi_pass", s->wifi_pass) == ESP_OK &&
              nvs_set_str(h, "password", s->password) == ESP_OK &&
              nvs_set_str(h, "sk_host", s->sk_host) == ESP_OK &&
              nvs_set_i32(h, "sk_port", s->sk_port) == ESP_OK &&
              nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (ok) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_set = *s;
        xSemaphoreGive(s_lock);
    }
    ESP_LOGI(TAG, "saved: %s", ok ? "ok" : "failed");
    return ok;
}
