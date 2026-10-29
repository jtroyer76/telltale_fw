// main.c — Telltale: board bring-up, config, screens, WiFi, OTA, Signal K.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "alerts.h"
#include "api.h"
#include "app.h"
#include "board.h"
#include "config.h"
#include "config_store.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fs.h"
#include "lvgl_port.h"
#include "nvs_flash.h"
#include "ota.h"
#include "screens.h"
#include "settings.h"
#include "setup.h"
#include "sk_client.h"
#include "store.h"
#include "tides.h"
#include "widgets.h"
#include "wifi.h"

static const char *TAG = "main";

// Crash-loop guard: a config that parses but crashes the firmware must not lock the device out.
// Survives panics and watchdog resets, not power cycles.
#define CRASH_MAGIC          0x7e117a1e
#define CRASH_CLEAR_AFTER_S  60
#define CRASHES_SKIP_SAVED   2   // then skip config.json
#define CRASHES_SKIP_LKG     4   // then skip config.lkg.json too
#define CRASHES_NO_SCREENS   6   // then don't build screens at all; WiFi + OTA only
static RTC_NOINIT_ATTR uint32_t s_crash_magic;
static RTC_NOINIT_ATTR uint32_t s_crashes;

static config_t *s_cfg;   // running config; the screens point into it

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

static int count_crashes(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    bool crashed = r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT;
    if (s_crash_magic != CRASH_MAGIC || !crashed) s_crashes = 0;
    if (crashed) s_crashes++;
    s_crash_magic = CRASH_MAGIC;
    if (s_crashes) ESP_LOGW(TAG, "%u crash(es) in a row", (unsigned)s_crashes);
    return s_crashes;
}

static void crash_clear_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(CRASH_CLEAR_AFTER_S * 1000));
    s_crashes = 0;
    vTaskDelete(NULL);
}

static void start_web(const char *hostname)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.recv_wait_timeout = 10;
    cfg.stack_size = 8192;   // config parsing runs in the handler
    cfg.max_uri_handlers = 16;
    cfg.lru_purge_enable = true;   // a phone on the setup hotspot opens many connections
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));
    ota_register(server);
    api_register(server);
    setup_register(server, hostname);
}

config_t *app_new_config(void)
{
    return heap_caps_calloc(1, sizeof(config_t), MALLOC_CAP_SPIRAM);
}

// A server in the settings (setup page) wins over the config's.
static void sk_target(const config_t *cfg, char *host, int host_len, int *port)
{
    settings_t set;
    settings_get(&set);
    snprintf(host, host_len, "%s", set.sk_host[0] ? set.sk_host : cfg->sk_host);
    *port = set.sk_host[0] ? set.sk_port : cfg->sk_port;
}

bool app_apply_config(config_t *cfg)
{
    config_t *old = s_cfg;
    if (lvgl_port_lock(-1)) {
        store_reset();
        alerts_reset();
        s_cfg = cfg;
        screens_set_notice("");
        screens_build(cfg);
        lvgl_port_unlock();
    }
    char host[SETTINGS_HOST_LEN];
    int port;
    sk_target(cfg, host, sizeof(host), &port);
    sk_client_set_server(host, port);   // re-subscribes; reconnects only if the server changed

    bool restart = old && strcmp(old->hostname, cfg->hostname) != 0;
    free(old);
    ESP_LOGI(TAG, "config applied: %d screens, %d paths", cfg->screen_count, store_watch_count());
    return restart;
}

void app_main(void)
{
    init_nvs();
    settings_init();
    store_init();
    alerts_init();
    tides_init();
    int crashes = count_crashes();
    ESP_ERROR_CHECK(board_init());
    if (!widgets_check_registry()) ESP_LOGE(TAG, "widget registry incomplete");
    fs_init();

    s_cfg = app_new_config();
    char note[192];
    int skip = crashes >= CRASHES_SKIP_LKG ? 2 : (crashes >= CRASHES_SKIP_SAVED ? 1 : 0);
    config_source_t src = config_store_load(s_cfg, skip, note, sizeof(note));
    bool ok = src != CFG_SRC_NONE;
    ESP_LOGI(TAG, "config: %s%s%s", config_source_name(src), note[0] ? " - " : "", note);

    if (lvgl_port_lock(-1)) {
        if (ok && crashes < CRASHES_NO_SCREENS) {
            screens_set_notice(note);
            screens_build(s_cfg);
        } else {
            screens_show_error(ok ? "Crashed repeatedly; screens are off until a new config or firmware is sent."
                                  : note);
        }
        lvgl_port_unlock();
    }

    // WiFi and OTA start even with a bad config, so the device can always be updated.
    const char *hostname = ok ? s_cfg->hostname : "telltale";
    wifi_start(hostname);
    start_web(hostname);
    ota_start_health_check();
    xTaskCreate(crash_clear_task, "crash_clear", 2048, NULL, 1, NULL);

    if (!ok) return;
    char host[SETTINGS_HOST_LEN];
    int port;
    sk_target(s_cfg, host, sizeof(host), &port);
    sk_client_start(host, port);
}
