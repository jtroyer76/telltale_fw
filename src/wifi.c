#include "wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "captive_dns.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "settings.h"

#define CMD_START  1
#define CMD_STOP   2
#define AP_PW_LEN  8
static const char AP_PW_CHARS[] = "abcdefghjkmnpqrstuvwxyz23456789";   // read off the screen

static const char *TAG = "wifi";
static volatile bool s_connected;
static volatile bool s_setup;         // hotspot on; don't try the saved network
static char s_ip[16];
static int s_disconnects, s_last_reason;
static bool s_have_saved;
static char s_saved[33];
static char s_ap_ssid[33], s_ap_pass[AP_PW_LEN + 1];
static char s_nets[SETUP_MAX_NETS][33];
static int s_net_count;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static int64_t s_down_since_us;      // saved network not joined since (0 = joined)

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_have_saved && !s_setup) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
        if (s_connected) s_disconnects++;
        if (s_connected || !s_down_since_us) s_down_since_us = esp_timer_get_time();
        s_last_reason = ev->reason;
        s_connected = false;
        s_ip[0] = '\0';
        if (!s_setup) {
            ESP_LOGW(TAG, "disconnected (reason %d), retrying", ev->reason);
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_connected = true;
        s_down_since_us = 0;
        ESP_LOGI(TAG, "got IP %s", s_ip);
    }
}

// What's in range, for the setup page. Done before the hotspot comes on: a scan hops channels.
static void scan(void)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < 10 && err != ESP_OK; i++) {   // a connect attempt may still be running
        err = esp_wifi_scan_start(&sc, true);
        if (err != ESP_OK) vTaskDelay(pdMS_TO_TICKS(500));
    }
    uint16_t n = 24;
    wifi_ap_record_t *recs = heap_caps_malloc(sizeof(wifi_ap_record_t) * n, MALLOC_CAP_SPIRAM);
    if (err != ESP_OK || !recs || esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) n = 0;
    if (!n) esp_wifi_clear_ap_list();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_net_count = 0;
    for (int i = 0; i < n && s_net_count < SETUP_MAX_NETS; i++) {   // records come strongest first
        const char *name = (const char *)recs[i].ssid;
        bool dup = !name[0];
        for (int j = 0; j < s_net_count && !dup; j++) dup = strcmp(s_nets[j], name) == 0;
        if (!dup) snprintf(s_nets[s_net_count++], sizeof(s_nets[0]), "%.32s", name);
    }
    xSemaphoreGive(s_lock);
    free(recs);
    ESP_LOGI(TAG, "scan: %d networks", s_net_count);
}

static void setup_on(void)
{
    s_setup = true;
    esp_wifi_disconnect();
    scan();
    wifi_config_t ap = { 0 };
    ap.ap.ssid_len = strlen(s_ap_ssid);
    memcpy(ap.ap.ssid, s_ap_ssid, ap.ap.ssid_len);
    memcpy(ap.ap.password, s_ap_pass, strlen(s_ap_pass));
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    captive_dns_start();
    ESP_LOGI(TAG, "setup hotspot on: %s, password %s", s_ap_ssid, s_ap_pass);   // serial = at the display
}

static void setup_off(const char *why)
{
    captive_dns_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_setup = false;
    s_down_since_us = esp_timer_get_time();
    esp_wifi_connect();
    ESP_LOGI(TAG, "setup hotspot off (%s), trying %s", why, s_saved);
}

static int phones(void)
{
    wifi_sta_list_t list;
    return s_setup && esp_wifi_ap_get_sta_list(&list) == ESP_OK ? list.num : 0;
}

static void wifi_task(void *arg)
{
    int64_t idle_since = 0;   // hotspot on with no phone since
    uint32_t cmd = s_have_saved ? 0 : CMD_START;
    for (;;) {
        int64_t now = esp_timer_get_time();
        if (!s_setup && s_have_saved && !s_connected && s_down_since_us &&
            now - s_down_since_us > (int64_t)SETUP_AFTER_S * 1000000) {
            ESP_LOGW(TAG, "%s not joined for %d s", s_saved, SETUP_AFTER_S);
            cmd = CMD_START;
        }
        if (cmd == CMD_START && !s_setup) {
            if (s_connected) vTaskDelay(pdMS_TO_TICKS(1000));   // let the reply to whoever asked go out first
            setup_on();
            idle_since = esp_timer_get_time();
        }
        if (s_setup && phones()) idle_since = now;
        if (s_setup && s_have_saved && (cmd == CMD_STOP || now - idle_since > (int64_t)SETUP_IDLE_S * 1000000)) {
            setup_off(cmd == CMD_STOP ? "cancelled" : "no phone");
        }
        cmd = 0;
        xTaskNotifyWait(0, UINT32_MAX, &cmd, pdMS_TO_TICKS(1000));
    }
}

void wifi_start(const char *hostname)
{
    s_lock = xSemaphoreCreateMutex();
    settings_t set;
    settings_get(&set);
    s_have_saved = set.wifi_ssid[0] != '\0';
    snprintf(s_saved, sizeof(s_saved), "%s", set.wifi_ssid);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(netif, hostname);
    esp_netif_create_default_wifi_ap();   // 192.168.4.1, with a DHCP server for the phone

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    esp_wifi_set_storage(WIFI_STORAGE_RAM);   // settings.h keeps the network, not the WiFi driver
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "Telltale-%02X%02X", mac[4], mac[5]);
    for (int i = 0; i < AP_PW_LEN; i++) s_ap_pass[i] = AP_PW_CHARS[esp_random() % (sizeof(AP_PW_CHARS) - 1)];
    s_ap_pass[AP_PW_LEN] = '\0';

    wifi_config_t cfg = { 0 };
    // Driver fields: a 32-character name fills ssid with no NUL, which it allows.
    memcpy(cfg.sta.ssid, set.wifi_ssid, strnlen(set.wifi_ssid, sizeof(cfg.sta.ssid)));
    memcpy(cfg.sta.password, set.wifi_pass, strnlen(set.wifi_pass, sizeof(cfg.sta.password) - 1));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    s_down_since_us = esp_timer_get_time();
    ESP_ERROR_CHECK(esp_wifi_start());
    // Mains-powered display: no modem sleep, for update speed and prompt Signal K data.
    esp_wifi_set_ps(WIFI_PS_NONE);

    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set(hostname);
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS: %s.local", hostname);

    xTaskCreate(wifi_task, "wifi_setup", 3072, NULL, 4, &s_task);
}

bool wifi_connected(void) { return s_connected; }

const char *wifi_ip(void) { return s_ip; }

bool wifi_link(int *rssi, int *channel)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return false;
    *rssi = ap.rssi;
    *channel = ap.primary;
    return true;
}

int wifi_disconnects(int *last_reason)
{
    *last_reason = s_last_reason;
    return s_disconnects;
}

void wifi_setup_start(void)
{
    if (s_task) xTaskNotify(s_task, CMD_START, eSetValueWithOverwrite);
}

void wifi_setup_stop(void)
{
    if (s_task) xTaskNotify(s_task, CMD_STOP, eSetValueWithOverwrite);
}

void wifi_setup_info(wifi_setup_t *out)
{
    *out = (wifi_setup_t){ .on = s_setup, .phones = phones(), .have_saved = s_have_saved };
    snprintf(out->ssid, sizeof(out->ssid), "%s", s_ap_ssid);
    snprintf(out->pass, sizeof(out->pass), "%s", s_ap_pass);
    snprintf(out->saved, sizeof(out->saved), "%s", s_saved);
}

int wifi_setup_networks(char names[][33], int max)
{
    if (!s_lock) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_net_count < max ? s_net_count : max;
    memcpy(names, s_nets, sizeof(s_nets[0]) * n);
    xSemaphoreGive(s_lock);
    return n;
}
