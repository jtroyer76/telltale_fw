/*
 * SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Board profile: Waveshare ESP32-S3-Touch-LCD-7 (800x480 RGB, GT911 touch, CH422G expander).
 * Adapted from waveshare_rgb_lcd_port.c in Waveshare's ESP32-S3-Touch-LCD-7 demo
 * (ESP-IDF/08_lvgl_Porting). Panel timing and pins are theirs, unchanged.
 */

#include "board.h"

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_rom_sys.h"
#include "lvgl_port.h"

static const char *TAG = "board";

#define I2C_MASTER_SCL_IO       9
#define I2C_MASTER_SDA_IO       8
#define I2C_MASTER_NUM          0
#define I2C_MASTER_FREQ_HZ      400000
#define I2C_MASTER_TIMEOUT_MS   1000
#define TOUCH_INT_GPIO          4

// CH422G: 0x24 = mode register, 0x38 = output register (EXIO pins)
#define CH422G_MODE_ADDR        0x24
#define CH422G_OUT_ADDR         0x38

#define LCD_PIXEL_CLOCK_HZ      (16 * 1000 * 1000)
#define RGB_BOUNCE_BUFFER_SIZE  (BOARD_H_RES * CONFIG_EXAMPLE_LCD_RGB_BOUNCE_BUFFER_HEIGHT)

IRAM_ATTR static bool on_vsync(esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_data_t *edata, void *user_ctx)
{
    return lvgl_port_notify_rgb_vsync();
}

static void ch422g_write(uint8_t addr, uint8_t value)
{
    i2c_master_write_to_device(I2C_MASTER_NUM, addr, &value, 1, I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
}

static esp_err_t i2c_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    i2c_param_config(I2C_MASTER_NUM, &conf);
    return i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
}

// GT911 reset through the CH422G, with INT held low to select the I2C address (Waveshare sequence).
static void touch_reset(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << TOUCH_INT_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);

    ch422g_write(CH422G_MODE_ADDR, 0x01);
    ch422g_write(CH422G_OUT_ADDR, 0x2C);
    esp_rom_delay_us(100 * 1000);
    gpio_set_level(TOUCH_INT_GPIO, 0);
    esp_rom_delay_us(100 * 1000);
    ch422g_write(CH422G_OUT_ADDR, 0x2E);
    esp_rom_delay_us(200 * 1000);
}

esp_err_t board_init(void)
{
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_rgb_panel_config_t panel_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = LCD_PIXEL_CLOCK_HZ,
            .h_res = BOARD_H_RES,
            .v_res = BOARD_V_RES,
            .hsync_pulse_width = 4,
            .hsync_back_porch = 8,
            .hsync_front_porch = 8,
            .vsync_pulse_width = 4,
            .vsync_back_porch = 8,
            .vsync_front_porch = 8,
            .flags = { .pclk_active_neg = 1 },
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = LVGL_PORT_LCD_RGB_BUFFER_NUMS,
        .bounce_buffer_size_px = RGB_BOUNCE_BUFFER_SIZE,
        .sram_trans_align = 4,
        .psram_trans_align = 64,
        .hsync_gpio_num = GPIO_NUM_46,
        .vsync_gpio_num = GPIO_NUM_3,
        .de_gpio_num = GPIO_NUM_5,
        .pclk_gpio_num = GPIO_NUM_7,
        .disp_gpio_num = -1,
        .data_gpio_nums = {
            GPIO_NUM_14, GPIO_NUM_38, GPIO_NUM_18, GPIO_NUM_17,
            GPIO_NUM_10, GPIO_NUM_39, GPIO_NUM_0,  GPIO_NUM_45,
            GPIO_NUM_48, GPIO_NUM_47, GPIO_NUM_21, GPIO_NUM_1,
            GPIO_NUM_2,  GPIO_NUM_42, GPIO_NUM_41, GPIO_NUM_40,
        },
        .flags = { .fb_in_psram = 1 },
    };
    ESP_LOGI(TAG, "RGB panel");
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    // PCLK's fast edges radiate into the 2.4 GHz antenna: at the default drive WiFi lost 10-60%
    // of pings. Weakest drive fixed it, measured 2026-10-28 (SPEC §5.5).
    gpio_set_drive_capability(GPIO_NUM_7, GPIO_DRIVE_CAP_0);

    ESP_LOGI(TAG, "I2C + touch");
    ESP_ERROR_CHECK(i2c_init());
    touch_reset();

    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    tp_io_config.scl_speed_hz = 0;   // the GT911 macro sets it, but the legacy I2C driver rejects it
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)I2C_MASTER_NUM, &tp_io_config, &tp_io));

    esp_lcd_touch_handle_t tp = NULL;
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BOARD_H_RES,
        .y_max = BOARD_V_RES,
        .rst_gpio_num = -1,
        .int_gpio_num = -1,
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &tp));

    ESP_ERROR_CHECK(lvgl_port_init(panel, tp));

    esp_lcd_rgb_panel_event_callbacks_t cbs = {
#if RGB_BOUNCE_BUFFER_SIZE > 0
        .on_bounce_frame_finish = on_vsync,
#else
        .on_vsync = on_vsync,
#endif
    };
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(panel, &cbs, NULL));
    return ESP_OK;
}

void board_backlight(bool on)
{
    ch422g_write(CH422G_MODE_ADDR, 0x01);
    ch422g_write(CH422G_OUT_ADDR, on ? 0x1E : 0x1A);
}
