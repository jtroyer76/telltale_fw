// board_crowpanel101.c — board profile: Elecrow CrowPanel Advanced 10.1" ESP32-P4, hardware V1.2
// (1024x600 MIPI-DSI panel with an EK79007 controller, GT911 touch, ESP32-C6 for WiFi over SDIO).
//
// Written for this project against the ESP-IDF and Espressif component APIs. The pins, panel timing
// and power-up order are facts read from Elecrow's V1.2 schematic and ESP-IDF lessons (SPEC §3.5
// lists each with its source); no Elecrow code is used.

#include "board.h"

#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_hosted.h"
#include "esp_lcd_ek79007.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "lvgl_port.h"

static const char *TAG = "board";

// The P4's own regulators: channel 3 feeds the MIPI D-PHY, channel 4 the 3.3 V IO domain.
#define LDO_MIPI_PHY_CHAN       3
#define LDO_MIPI_PHY_MV         2500
#define LDO_IO_CHAN             4
#define LDO_IO_MV               3300

#define I2C_SDA_GPIO            45
#define I2C_SCL_GPIO            46
#define I2C_FREQ_HZ             400000
#define TOUCH_RST_GPIO          40

#define DSI_LANES               2
#define DSI_LANE_MBPS           900
#define DPI_CLOCK_MHZ           51

// Backlight: the MT9201 LED driver's enable pin, dimmed by PWM. GPIO29 (a backlight power gate in
// the schematic) is not driven: its MOSFET is unpopulated on V1.2 and a 0R link feeds the driver.
#define BACKLIGHT_GPIO          31
#define BACKLIGHT_PWM_HZ        30000
#define BACKLIGHT_DUTY_BITS     LEDC_TIMER_11_BIT
#define BACKLIGHT_DUTY_ON       2000   // of 2047; the top of the range Elecrow's lessons use

static esp_err_t power_init(void)
{
    static esp_ldo_channel_handle_t phy, io;
    const esp_ldo_channel_config_t phy_cfg = { .chan_id = LDO_MIPI_PHY_CHAN, .voltage_mv = LDO_MIPI_PHY_MV };
    const esp_ldo_channel_config_t io_cfg = { .chan_id = LDO_IO_CHAN, .voltage_mv = LDO_IO_MV };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&phy_cfg, &phy), TAG, "MIPI PHY LDO");
    return esp_ldo_acquire_channel(&io_cfg, &io);
}

static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = BACKLIGHT_DUTY_BITS,
        .freq_hz = BACKLIGHT_PWM_HZ,
        .clk_cfg = LEDC_USE_PLL_DIV_CLK,
    };
    const ledc_channel_config_t channel = {
        .gpio_num = BACKLIGHT_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,   // dark until the first frame is drawn
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer");
    return ledc_channel_config(&channel);
}

static esp_err_t panel_init(esp_lcd_panel_handle_t *panel)
{
    esp_lcd_dsi_bus_handle_t bus = NULL;
    const esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = DSI_LANES,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = DSI_LANE_MBPS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &bus), TAG, "DSI bus");

    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_dbi_io_config_t io_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(bus, &io_cfg, &io), TAG, "DSI command channel");

    const esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = DPI_CLOCK_MHZ,
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .num_fbs = 1,
        .video_timing = {
            .h_size = BOARD_H_RES,
            .v_size = BOARD_V_RES,
            .hsync_back_porch = 160,
            .hsync_pulse_width = 70,
            .hsync_front_porch = 160,
            .vsync_back_porch = 23,
            .vsync_pulse_width = 10,
            .vsync_front_porch = 12,
        },
        .flags.use_dma2d = true,
    };
    ek79007_vendor_config_t vendor_cfg = {
        .mipi_config = {
            .dsi_bus = bus,
            .dpi_config = &dpi_cfg,
        },
    };
    // No reset pin: the schematic routes LCD_RESET to GPIO41, but Elecrow's working code leaves it alone.
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ek79007(io, &panel_cfg, panel), TAG, "EK79007");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(*panel), TAG, "panel reset");
    return esp_lcd_panel_init(*panel);
}

// The GT911 answers at 0x5D or 0x14, set by the level of its INT pin as it leaves reset.
static esp_err_t touch_init_at(i2c_master_bus_handle_t bus, uint8_t addr, esp_lcd_touch_handle_t *tp)
{
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_cfg.dev_addr = addr;
    io_cfg.scl_speed_hz = I2C_FREQ_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io), TAG, "touch I2C");

    esp_lcd_touch_io_gt911_config_t gt911_cfg = { .dev_addr = addr };
    const esp_lcd_touch_config_t cfg = {
        .x_max = BOARD_H_RES,
        .y_max = BOARD_V_RES,
        .rst_gpio_num = TOUCH_RST_GPIO,
        .int_gpio_num = -1,   // polled
        .driver_data = &gt911_cfg,
    };
    esp_err_t err = esp_lcd_touch_new_i2c_gt911(io, &cfg, tp);
    if (err != ESP_OK) esp_lcd_panel_io_del(io);
    return err;
}

static esp_err_t touch_init(esp_lcd_touch_handle_t *tp)
{
    i2c_master_bus_handle_t bus = NULL;
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus), TAG, "I2C bus");

    esp_err_t err = touch_init_at(bus, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS, tp);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no GT911 at 0x%02x, trying 0x%02x", ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
                 ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP);
        err = touch_init_at(bus, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP, tp);
    }
    return err;
}

esp_err_t board_init(void)
{
    ESP_RETURN_ON_ERROR(power_init(), TAG, "LDOs");

    // WiFi runs on the ESP32-C6. esp-hosted starts its link from a constructor, which PlatformIO's
    // link step drops, so start it here; esp_wifi_init() then waits for the C6 to answer.
    ESP_RETURN_ON_ERROR(esp_hosted_init(), TAG, "esp-hosted");
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight");

    // The display must work without touch: a dead touch panel still shows data.
    esp_lcd_touch_handle_t tp = NULL;
    if (touch_init(&tp) != ESP_OK) {
        ESP_LOGE(TAG, "touch not found");
        tp = NULL;
    }

    esp_lcd_panel_handle_t panel = NULL;
    ESP_LOGI(TAG, "MIPI-DSI panel");
    ESP_RETURN_ON_ERROR(panel_init(&panel), TAG, "panel");
    ESP_RETURN_ON_ERROR(lvgl_port_init(panel, tp), TAG, "LVGL");

    board_backlight(true);
    return ESP_OK;
}

void board_backlight(bool on)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, on ? BACKLIGHT_DUTY_ON : 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}
