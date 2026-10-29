#include "ota.h"

#include <stdio.h>
#include <string.h>
#include "auth.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi.h"

static const char *TAG = "ota";

#define CHUNK_SIZE         4096
#define MAX_RECV_TIMEOUTS  3      // x recv_wait_timeout (10 s) of silence ends an upload

static volatile int s_progress = -1;

int ota_progress(void) { return s_progress; }

static esp_err_t ota_post(httpd_req_t *req)
{
    if (!auth_check(req)) return ESP_FAIL;

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
        return ESP_FAIL;
    }
    if (req->content_len > target->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "image larger than the OTA partition");
        return ESP_FAIL;
    }

    esp_ota_handle_t handle;
    if (esp_ota_begin(target, OTA_SIZE_UNKNOWN, &handle) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_begin failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "receiving %d bytes into %s", req->content_len, target->label);

    char *buf = heap_caps_malloc(CHUNK_SIZE, MALLOC_CAP_INTERNAL);
    size_t remaining = req->content_len;
    s_progress = 0;
    int timeouts = 0;
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf, remaining < CHUNK_SIZE ? remaining : CHUNK_SIZE);
        // A sender that vanished without closing would otherwise hold the (only) HTTP task forever.
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < MAX_RECV_TIMEOUTS) continue;
        timeouts = 0;
        if (n <= 0 || esp_ota_write(handle, buf, n) != ESP_OK) {
            ESP_LOGE(TAG, "upload failed with %u bytes left", (unsigned)remaining);
            esp_ota_abort(handle);
            free(buf);
            s_progress = -1;
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upload failed");
            return ESP_FAIL;
        }
        remaining -= n;
        s_progress = (int)(100 - (100LL * remaining) / req->content_len);
    }
    free(buf);

    // esp_ota_end checks the image (magic, chip, checksum) before it can be booted.
    if (esp_ota_end(handle) != ESP_OK || esp_ota_set_boot_partition(target) != ESP_OK) {
        s_progress = -1;
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "image rejected");
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "OK, restarting\n");
    ESP_LOGI(TAG, "update written, restarting");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t info_get(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    char body[160];
    snprintf(body, sizeof(body), "{\"version\":\"%s\",\"partition\":\"%s\",\"ip\":\"%s\"}\n",
             app->version, running->label, wifi_ip());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

void ota_register(httpd_handle_t server)
{
    static const httpd_uri_t ota = { .uri = "/ota", .method = HTTP_POST, .handler = ota_post };
    static const httpd_uri_t info = { .uri = "/api/info", .method = HTTP_GET, .handler = info_get };
    httpd_register_uri_handler(server, &ota);
    httpd_register_uri_handler(server, &info);
}

static void health_task(void *arg)
{
    for (int s = 0; s < OTA_HEALTHY_TIMEOUT_S; s++) {
        if (wifi_connected()) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "new image confirmed");
            vTaskDelete(NULL);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGE(TAG, "no WiFi within %d s of an update, rolling back", OTA_HEALTHY_TIMEOUT_S);
    esp_ota_mark_app_invalid_rollback_and_reboot();
}

void ota_start_health_check(void)
{
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "first boot of a new image, waiting for WiFi to confirm it");
        xTaskCreate(health_task, "ota_health", 3072, NULL, 5, NULL);
    }
}
