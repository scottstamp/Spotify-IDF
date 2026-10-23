#include "cyd_display.h"
#include <string.h>
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_heap_caps.h"

static const char *TAG = "cyd_display";

static esp_lcd_panel_io_handle_t s_io_handle = NULL;
static esp_lcd_panel_handle_t s_panel_handle = NULL;
static lv_display_t *s_lvgl_disp = NULL;

static IRAM_ATTR bool on_color_trans_done(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    if (s_lvgl_disp) {
        lv_display_flush_ready(s_lvgl_disp);
    }
    return false;
}

void cyd_display_set_lvgl_disp(lv_display_t *disp)
{
    s_lvgl_disp = disp;
}

esp_lcd_panel_handle_t cyd_display_get_panel_handle(void)
{
    return s_panel_handle;
}

esp_err_t cyd_display_init(void)
{
    ESP_LOGI(TAG, "Initializing CYD Display for LVGL 9...");

    // Initialize Backlight pin
    gpio_config_t bl_conf = {
        .pin_bit_mask = (1ULL << CYD_PIN_LCD_BL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
    };
    gpio_config(&bl_conf);
    gpio_set_level(CYD_PIN_LCD_BL, 1); // Turn on backlight

    // Initialize and turn off onboard RGB LED (GPIOs 4, 16, 17 on low side as drains)
    cyd_rgb_led_init();

    // Configure SPI Bus for LCD (SPI2_HOST / HSPI)
    spi_bus_config_t buscfg = {
        .sclk_io_num = CYD_PIN_LCD_SCLK,
        .mosi_io_num = CYD_PIN_LCD_MOSI,
        .miso_io_num = CYD_PIN_LCD_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = CYD_LCD_WIDTH * 40 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // Configure LCD IO Panel SPI
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = CYD_PIN_LCD_DC,
        .cs_gpio_num = CYD_PIN_LCD_CS,
        .pclk_hz = 40 * 1000 * 1000, // 40 MHz SPI
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = on_color_trans_done,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &s_io_handle));

    // Configure ST7789/ILI9341 Panel
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = CYD_PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(s_io_handle, &panel_config, &s_panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel_handle));

    // Landscape orientation: swap XY = true, mirror X = false, mirror Y = true
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel_handle, false, true));

    // Normal color mode (Inversion OFF for dark theme & true colors)
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel_handle, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel_handle, true));

    ESP_LOGI(TAG, "CYD Display hardware initialized successfully");
    return ESP_OK;
}

void cyd_display_set_backlight(uint8_t brightness_pct)
{
    gpio_set_level(CYD_PIN_LCD_BL, brightness_pct > 0 ? 1 : 0);
}

void cyd_rgb_led_init(void)
{
    gpio_config_t rgb_conf = {
        .pin_bit_mask = (1ULL << CYD_PIN_RGB_LED_R) |
                        (1ULL << CYD_PIN_RGB_LED_G) |
                        (1ULL << CYD_PIN_RGB_LED_B),
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&rgb_conf);

    // Common anode: GPIOs on low side act as drains (active LOW).
    // Set HIGH (1) to disable current sinking and turn LED completely off.
    cyd_rgb_led_off();
    ESP_LOGI(TAG, "Onboard RGB LED initialized and turned OFF (R:IO%d, G:IO%d, B:IO%d)",
             CYD_PIN_RGB_LED_R, CYD_PIN_RGB_LED_G, CYD_PIN_RGB_LED_B);
}

void cyd_rgb_led_set(bool r, bool g, bool b)
{
    // Active low: 0 turns ON, 1 turns OFF
    gpio_set_level(CYD_PIN_RGB_LED_R, r ? 0 : 1);
    gpio_set_level(CYD_PIN_RGB_LED_G, g ? 0 : 1);
    gpio_set_level(CYD_PIN_RGB_LED_B, b ? 0 : 1);
}

void cyd_rgb_led_off(void)
{
    cyd_rgb_led_set(false, false, false);
}
