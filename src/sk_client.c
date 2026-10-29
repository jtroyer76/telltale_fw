#include "sk_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "alerts.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "lwip/ip4_addr.h"
#include "mdns.h"
#include "sk_time.h"
#include "store.h"
#include "units.h"

static const char *TAG = "sk";

#define MSG_BUF_SIZE        16384   // one assembled WebSocket message
#define SUBSCRIBE_PERIOD_MS 1000
#define MDNS_RETRY_MS       10000
#define FUTURE_SLACK_MS     2000    // server timestamps further ahead than this rebase the clock
// A fresh connection always gets a hello and the subscribed values' last state, so a
// "connected" stream that says nothing this long is dead: reconnect, and if even that
// doesn't help (client stuck), restart. Seen 2026-10-28: desk relay dropped the TCP
// connection, the client stayed "connected", pings never timed out.
#define SILENT_RECONNECT_S  60
#define SILENT_RESTART_S    180
#define WATCH_PERIOD_MS     10000

#define NOTIFY_SUBSCRIBE    (1u << 0)
#define NOTIFY_SERVER       (1u << 1)

static esp_websocket_client_handle_t s_client;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_server_lock;
static char s_host[64];          // wanted server ("" = mDNS)
static int s_port;
static char s_active[80];        // "host:port" connected to or being tried
static volatile bool s_connected;
static volatile int64_t s_last_msg_us;
static char *s_buf;
static int s_buf_len;

// Server clock minus our clock, from the hello message; values' ages come from server timestamps.
static bool s_have_offset;
static int64_t s_offset_ms;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

// When the server got a value, in our esp_timer microseconds.
static int64_t received_us(const cJSON *timestamp)
{
    int64_t ts;
    if (!s_have_offset || !cJSON_IsString(timestamp) || !sk_time_parse(timestamp->valuestring, &ts)) {
        return esp_timer_get_time();
    }
    int64_t local = ts - s_offset_ms;
    if (local > now_ms() + FUTURE_SLACK_MS) {
        // Server clock jumped forward (e.g. the Pi got NTP time after we connected).
        s_offset_ms = ts - now_ms();
        store_set_clock(true, s_offset_ms);
        local = now_ms();
    }
    return local * 1000;
}

static void add_sub(const char *path, void *ctx)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "path", path);
    cJSON_AddNumberToObject(s, "period", SUBSCRIBE_PERIOD_MS);
    cJSON_AddItemToArray((cJSON *)ctx, s);
}

static void send_json(cJSON *msg)
{
    char *text = cJSON_PrintUnformatted(msg);
    esp_websocket_client_send_text(s_client, text, strlen(text), pdMS_TO_TICKS(5000));
    cJSON_free(text);
    cJSON_Delete(msg);
}

static void subscribe(void)
{
    if (!s_client || !s_connected) return;

    cJSON *unsub = cJSON_CreateObject();
    cJSON_AddStringToObject(unsub, "context", "*");
    cJSON *all = cJSON_CreateObject();
    cJSON_AddStringToObject(all, "path", "*");
    cJSON_AddItemToArray(cJSON_AddArrayToObject(unsub, "unsubscribe"), all);
    send_json(unsub);

    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "context", "vessels.self");
    cJSON *subs = cJSON_AddArrayToObject(msg, "subscribe");
    int n = store_for_each_subscribed(add_sub, subs);
    n += alerts_for_each_subscribed(store_active_screen(), add_sub, subs);
    if (n > 0) {
        send_json(msg);
    } else {
        cJSON_Delete(msg);
    }
    ESP_LOGI(TAG, "subscribed to %d paths (screen %d)", n, store_active_screen());
}

static void handle_meta(const cJSON *path, const cJSON *value)
{
    if (!cJSON_IsString(path) || !cJSON_IsObject(value)) return;
    store_meta_t m = { .has_meta = true, .decimals = -1 };

    const cJSON *units = cJSON_GetObjectItemCaseSensitive(value, "units");
    const cJSON *display = cJSON_GetObjectItemCaseSensitive(value, "displayUnits");
    const cJSON *target = cJSON_GetObjectItemCaseSensitive(display, "targetUnit");
    const cJSON *format = cJSON_GetObjectItemCaseSensitive(display, "displayFormat");
    const char *unit = units_from_sk(cJSON_IsString(units) ? units->valuestring : NULL,
                                     cJSON_IsString(target) ? target->valuestring : NULL);
    if (cJSON_IsString(units)) snprintf(m.si, sizeof(m.si), "%s", units->valuestring);
    if (unit) snprintf(m.unit, sizeof(m.unit), "%s", unit);
    if (cJSON_IsString(format)) m.decimals = units_decimals_from_format(format->valuestring);

    const cJSON *name = cJSON_GetObjectItemCaseSensitive(value, "displayName");
    if (cJSON_IsString(name)) snprintf(m.display_name, sizeof(m.display_name), "%s", name->valuestring);

    const cJSON *z;
    cJSON_ArrayForEach(z, cJSON_GetObjectItemCaseSensitive(value, "zones")) {
        if (m.zone_count >= CFG_MAX_ZONES) break;
        zone_t *out = &m.zones[m.zone_count];
        const cJSON *lower = cJSON_GetObjectItemCaseSensitive(z, "lower");
        const cJSON *upper = cJSON_GetObjectItemCaseSensitive(z, "upper");
        const cJSON *state = cJSON_GetObjectItemCaseSensitive(z, "state");
        out->has_lower = cJSON_IsNumber(lower);
        out->has_upper = cJSON_IsNumber(upper);
        out->lower = out->has_lower ? lower->valuedouble : 0;
        out->upper = out->has_upper ? upper->valuedouble : 0;
        if ((out->has_lower || out->has_upper) && cJSON_IsString(state) &&
            zone_state_from_name(state->valuestring, &out->state)) {
            m.zone_count++;
        }
    }
    store_set_meta(path->valuestring, &m);
}

// notifications.<path>: {state, message, ...}, or null when the notification is removed.
static void handle_notification(const char *path, const cJSON *value)
{
    if (cJSON_IsNull(value)) {
        alerts_set(path, NULL, NULL);
    } else if (cJSON_IsObject(value)) {
        const cJSON *state = cJSON_GetObjectItemCaseSensitive(value, "state");
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(value, "message");
        alerts_set(path, cJSON_IsString(state) ? state->valuestring : "alert",
                   cJSON_IsString(message) ? message->valuestring : "");
    }
}

static void handle_value(const cJSON *path, const cJSON *value, int64_t at_us)
{
    if (!cJSON_IsString(path)) return;
    if (strncmp(path->valuestring, "notifications.", 14) == 0) {
        handle_notification(path->valuestring + 14, value);
        return;
    }
    if (cJSON_IsNumber(value)) {
        store_set_number(path->valuestring, value->valuedouble, at_us);
    } else if (cJSON_IsString(value)) {
        store_set_text(path->valuestring, value->valuestring, at_us);
    } else if (cJSON_IsBool(value)) {
        store_set_text(path->valuestring, cJSON_IsTrue(value) ? "true" : "false", at_us);   // e.g. switch states
    } else if (cJSON_IsObject(value)) {
        // Positions (navigation.position, navigation.anchor.position). Other objects aren't shown.
        const cJSON *lat = cJSON_GetObjectItemCaseSensitive(value, "latitude");
        const cJSON *lon = cJSON_GetObjectItemCaseSensitive(value, "longitude");
        if (cJSON_IsNumber(lat) && cJSON_IsNumber(lon)) {
            store_set_position(path->valuestring, lat->valuedouble, lon->valuedouble, at_us);
        }
    } else if (cJSON_IsNull(value)) {
        store_set_null(path->valuestring, at_us);
    }
}

static void handle_hello(const cJSON *root)
{
    const cJSON *ts = cJSON_GetObjectItemCaseSensitive(root, "timestamp");
    int64_t server_ms;
    if (cJSON_IsString(ts) && sk_time_parse(ts->valuestring, &server_ms)) {
        s_offset_ms = server_ms - now_ms();
        s_have_offset = true;
        store_set_clock(true, s_offset_ms);
    } else {
        s_have_offset = false;
        ESP_LOGW(TAG, "hello has no timestamp; ages count from arrival");
    }
}

static void handle_message(const char *text)
{
    cJSON *root = cJSON_Parse(text);
    if (!root) return;
    s_last_msg_us = esp_timer_get_time();

    const cJSON *updates = cJSON_GetObjectItemCaseSensitive(root, "updates");
    if (!updates && cJSON_GetObjectItemCaseSensitive(root, "self")) handle_hello(root);

    const cJSON *update;
    cJSON_ArrayForEach(update, updates) {
        int64_t at_us = received_us(cJSON_GetObjectItemCaseSensitive(update, "timestamp"));
        const cJSON *v;
        cJSON_ArrayForEach(v, cJSON_GetObjectItemCaseSensitive(update, "values")) {
            handle_value(cJSON_GetObjectItemCaseSensitive(v, "path"), cJSON_GetObjectItemCaseSensitive(v, "value"),
                         at_us);
        }
        cJSON_ArrayForEach(v, cJSON_GetObjectItemCaseSensitive(update, "meta")) {
            handle_meta(cJSON_GetObjectItemCaseSensitive(v, "path"), cJSON_GetObjectItemCaseSensitive(v, "value"));
        }
    }
    cJSON_Delete(root);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected");
        s_connected = true;
        s_last_msg_us = esp_timer_get_time();   // silence counts from the connection
        store_invalidate();
        alerts_set_live(true);                   // the subscribe below brings back what is active
        xTaskNotify(s_task, NOTIFY_SUBSCRIBE, eSetBits);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_ERROR:
        if (s_connected) ESP_LOGW(TAG, "disconnected");
        s_connected = false;
        alerts_set_live(false);
        break;
    case WEBSOCKET_EVENT_DATA:
        if (data->op_code != 0x1 && data->op_code != 0x0) break;   // text frames and continuations only
        if (data->payload_offset == 0) s_buf_len = 0;
        if (s_buf_len + data->data_len >= MSG_BUF_SIZE) {
            ESP_LOGW(TAG, "message too large (%d bytes), dropped", data->payload_len);
            s_buf_len = MSG_BUF_SIZE;   // drop the rest of this message
            break;
        }
        if (s_buf_len < MSG_BUF_SIZE) {
            memcpy(s_buf + s_buf_len, data->data_ptr, data->data_len);
            s_buf_len += data->data_len;
        }
        if (data->payload_offset + data->data_len >= data->payload_len && s_buf_len < MSG_BUF_SIZE) {
            s_buf[s_buf_len] = '\0';
            handle_message(s_buf);
        }
        break;
    default:
        break;
    }
}

static void ws_disconnect(void)
{
    if (!s_client) return;
    esp_websocket_client_stop(s_client);
    esp_websocket_client_destroy(s_client);
    s_client = NULL;
    s_connected = false;
    alerts_set_live(false);
}

static void ws_connect(const char *host, int port)
{
    char uri[160];
    snprintf(uri, sizeof(uri), "ws://%s:%d/signalk/v1/stream?subscribe=none&sendMeta=all", host, port);
    ESP_LOGI(TAG, "connecting to %s", uri);

    esp_websocket_client_config_t cfg = {
        .uri = uri,
        .buffer_size = 2048,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
        // A connection that stops answering pings is dropped and retried, so a dead link
        // can't sit there looking "connected" (seen once on the desk relay, 2026-10-28).
        .ping_interval_sec = 10,
        .pingpong_timeout_sec = 30,
    };
    s_client = esp_websocket_client_init(&cfg);
    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, on_event, NULL);
    esp_websocket_client_start(s_client);
}

// Signal K servers advertise _signalk-ws._tcp. **(verify on the boat)** — see SPEC §5.2.
static bool discover(char *host, int host_len, int *port)
{
    mdns_result_t *results = NULL;
    if (mdns_query_ptr("_signalk-ws", "_tcp", 3000, 4, &results) != ESP_OK || !results) return false;
    bool found = false;
    for (mdns_result_t *r = results; r && !found; r = r->next) {
        for (mdns_ip_addr_t *a = r->addr; a; a = a->next) {
            if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                snprintf(host, host_len, IPSTR, IP2STR(&a->addr.u_addr.ip4));
                *port = r->port;
                found = true;
                break;
            }
        }
    }
    mdns_query_results_free(results);
    return found;
}

static int silent_s(void) { return (int)((esp_timer_get_time() - s_last_msg_us) / 1000000); }

// Last resort, from the timer task: the reconnect above runs in sk_task, which a stuck
// client could block.
static void watchdog(TimerHandle_t t)
{
    if (s_connected && silent_s() > SILENT_RESTART_S) {
        ESP_LOGE(TAG, "Signal K silent for %d s after reconnecting, restarting", silent_s());
        esp_restart();
    }
}

static void sk_task(void *arg)
{
    uint32_t bits = NOTIFY_SERVER;
    for (;;) {
        if (bits & NOTIFY_SERVER) {
            ws_disconnect();
            char host[64];
            int port;
            xSemaphoreTake(s_server_lock, portMAX_DELAY);
            snprintf(host, sizeof(host), "%s", s_host);
            port = s_port;
            xSemaphoreGive(s_server_lock);

            if (host[0] == '\0') {
                snprintf(s_active, sizeof(s_active), "searching (mDNS)");
                bool restart = false;
                while (!restart && !discover(host, sizeof(host), &port)) {
                    // A server change while searching starts over; subscriptions wait for a connection.
                    uint32_t got = 0;
                    xTaskNotifyWait(0, UINT32_MAX, &got, pdMS_TO_TICKS(MDNS_RETRY_MS));
                    restart = got & NOTIFY_SERVER;
                }
                if (restart) {
                    bits = NOTIFY_SERVER;
                    continue;
                }
                ESP_LOGI(TAG, "found Signal K at %s:%d by mDNS", host, port);
            }
            snprintf(s_active, sizeof(s_active), "%s:%d", host, port);
            ws_connect(host, port);
        }
        if (bits & NOTIFY_SUBSCRIBE) subscribe();
        bits = 0;
        if (!xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(WATCH_PERIOD_MS)) &&
            s_connected && silent_s() > SILENT_RECONNECT_S) {
            ESP_LOGW(TAG, "connected but silent for %d s, reconnecting", silent_s());
            bits = NOTIFY_SERVER;
        }
    }
}

void sk_client_start(const char *host, int port)
{
    s_buf = heap_caps_malloc(MSG_BUF_SIZE, MALLOC_CAP_SPIRAM);
    s_server_lock = xSemaphoreCreateMutex();
    snprintf(s_host, sizeof(s_host), "%s", host);
    s_port = port;
    xTaskCreate(sk_task, "sk_client", 6144, NULL, 5, &s_task);
    xTimerStart(xTimerCreate("sk_watch", pdMS_TO_TICKS(WATCH_PERIOD_MS), pdTRUE, NULL, watchdog), 0);
}

void sk_client_set_server(const char *host, int port)
{
    if (!s_task) {   // never started (no config at boot)
        sk_client_start(host, port);
        return;
    }
    xSemaphoreTake(s_server_lock, portMAX_DELAY);
    bool same = strcmp(s_host, host) == 0 && s_port == port;
    snprintf(s_host, sizeof(s_host), "%s", host);
    s_port = port;
    xSemaphoreGive(s_server_lock);
    xTaskNotify(s_task, same ? NOTIFY_SUBSCRIBE : NOTIFY_SERVER, eSetBits);
}

void sk_client_resubscribe(void)
{
    if (s_task) xTaskNotify(s_task, NOTIFY_SUBSCRIBE, eSetBits);
}

bool sk_client_connected(void) { return s_connected; }

void sk_client_server(char *buf, int len) { snprintf(buf, len, "%s", s_active); }

int64_t sk_client_last_message_us(void) { return s_last_msg_us; }
