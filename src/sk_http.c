#include "sk_http.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "sk_client.h"

typedef struct {
    char *buf;
    int len;
    int max;
    bool overflow;
} body_t;

static esp_err_t on_http(esp_http_client_event_t *e)
{
    body_t *b = e->user_data;
    // Body bytes, chunked or not; only the final 200 response's (not a redirect's "See Other").
    if (e->event_id == HTTP_EVENT_ON_DATA && esp_http_client_get_status_code(e->client) == 200) {
        if (b->len + e->data_len >= b->max) {
            b->overflow = true;
        } else {
            memcpy(b->buf + b->len, e->data, e->data_len);
            b->len += e->data_len;
        }
    }
    return ESP_OK;
}

char *sk_http_get(const char *path, int max_bytes, const char *tag)
{
    char server[80], url[CFG_PATH_LEN * 2 + 160];
    sk_client_server(server, sizeof(server));
    snprintf(url, sizeof(url), "http://%s%s", server, path);
    body_t b = { .buf = heap_caps_malloc(max_bytes, MALLOC_CAP_SPIRAM), .max = max_bytes };
    if (!b.buf) return NULL;
    esp_http_client_config_t cfg = { .url = url, .timeout_ms = 15000, .event_handler = on_http, .user_data = &b };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    if (err != ESP_OK || status != 200 || b.overflow || b.len == 0) {
        ESP_LOGW(tag, "%s: %s, HTTP %d%s", path, esp_err_to_name(err), status, b.overflow ? ", too large" : "");
        free(b.buf);
        return NULL;
    }
    b.buf[b.len] = '\0';
    return b.buf;
}
