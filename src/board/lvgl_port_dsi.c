// lvgl_port_dsi.c — LVGL 8 on a MIPI-DSI video-mode panel (ESP32-P4). Implements lvgl_port.h for
// boards whose panel is not RGB; lvgl_port.c is the RGB one.
//
// LVGL draws into one strip buffer in internal RAM and each flush is copied into the panel's frame
// buffer (PSRAM) by the DSI driver. LVGL waits for that copy to finish before it reuses the strip.

#include "lvgl_port.h"

#include "board.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "lv_port";

// Height of the LVGL draw buffer. 32 lines (64 KB) in internal RAM: measured against 100 lines in
// PSRAM, a full-screen redraw took a third less time (SPEC §3.5).
#define STRIP_LINES 32

static SemaphoreHandle_t s_mux;
static lv_disp_drv_t s_disp_drv;

static bool on_color_trans_done(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx)
{
    lv_disp_flush_ready(&s_disp_drv);
    return false;
}

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel = drv->user_data;
    // lv_disp_flush_ready() comes from on_color_trans_done, once the copy is complete
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_map);
}

static esp_err_t display_init(esp_lcd_panel_handle_t panel)
{
    static lv_disp_draw_buf_t draw_buf;
    size_t px = BOARD_H_RES * STRIP_LINES;
    void *buf = heap_caps_aligned_calloc(64, px, sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!buf) return ESP_ERR_NO_MEM;
    lv_disp_draw_buf_init(&draw_buf, buf, NULL, px);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = BOARD_H_RES;
    s_disp_drv.ver_res = BOARD_V_RES;
    s_disp_drv.flush_cb = flush_cb;
    s_disp_drv.draw_buf = &draw_buf;
    s_disp_drv.user_data = panel;
    if (!lv_disp_drv_register(&s_disp_drv)) return ESP_FAIL;

    const esp_lcd_dpi_panel_event_callbacks_t cbs = { .on_color_trans_done = on_color_trans_done };
    return esp_lcd_dpi_panel_register_event_callbacks(panel, &cbs, NULL);
}

static void touch_read(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    esp_lcd_touch_handle_t tp = drv->user_data;
    uint16_t x, y;
    uint8_t count = 0;
    esp_lcd_touch_read_data(tp);
    if (esp_lcd_touch_get_coordinates(tp, &x, &y, NULL, &count, 1) && count > 0) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void tick_cb(void *arg)
{
    lv_tick_inc(LVGL_PORT_TICK_PERIOD_MS);
}

static void lvgl_task(void *arg)
{
    uint32_t delay_ms = LVGL_PORT_TASK_MAX_DELAY_MS;
    while (1) {
        if (lvgl_port_lock(-1)) {
            delay_ms = lv_timer_handler();
            lvgl_port_unlock();
        }
        if (delay_ms > LVGL_PORT_TASK_MAX_DELAY_MS) delay_ms = LVGL_PORT_TASK_MAX_DELAY_MS;
        if (delay_ms < LVGL_PORT_TASK_MIN_DELAY_MS) delay_ms = LVGL_PORT_TASK_MIN_DELAY_MS;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

esp_err_t lvgl_port_init(esp_lcd_panel_handle_t lcd_handle, esp_lcd_touch_handle_t tp_handle)
{
    lv_init();

    const esp_timer_create_args_t tick_args = { .callback = tick_cb, .name = "LVGL tick" };
    esp_timer_handle_t tick = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick, LVGL_PORT_TICK_PERIOD_MS * 1000));

    esp_err_t err = display_init(lcd_handle);
    if (err != ESP_OK) return err;

    if (tp_handle) {
        static lv_indev_drv_t indev_drv;
        lv_indev_drv_init(&indev_drv);
        indev_drv.type = LV_INDEV_TYPE_POINTER;
        indev_drv.read_cb = touch_read;
        indev_drv.user_data = tp_handle;
        if (!lv_indev_drv_register(&indev_drv)) return ESP_FAIL;
    }

    s_mux = xSemaphoreCreateRecursiveMutex();
    if (!s_mux) return ESP_ERR_NO_MEM;

    BaseType_t core = (LVGL_PORT_TASK_CORE < 0) ? tskNO_AFFINITY : LVGL_PORT_TASK_CORE;
    if (xTaskCreatePinnedToCore(lvgl_task, "lvgl", LVGL_PORT_TASK_STACK_SIZE, NULL,
                                LVGL_PORT_TASK_PRIORITY, NULL, core) != pdPASS) {
        ESP_LOGE(TAG, "no LVGL task");
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool lvgl_port_lock(int timeout_ms)
{
    assert(s_mux && "lvgl_port_init must be called first");
    const TickType_t ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_mux, ticks) == pdTRUE;
}

void lvgl_port_unlock(void)
{
    assert(s_mux && "lvgl_port_init must be called first");
    xSemaphoreGiveRecursive(s_mux);
}
