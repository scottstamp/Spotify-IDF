#include "cyd_touch.h"
#include <string.h>
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cyd_touch";

static spi_device_handle_t s_touch_spi = NULL;
static cyd_touch_callback_t s_touch_cb = NULL;

#define XPT_CMD_X 0xD0
#define XPT_CMD_Y 0x90
#define XPT_CMD_Z1 0xB0
#define XPT_CMD_Z2 0xC0

#define CAL_XMIN 185
#define CAL_XMAX 3700
#define CAL_YMIN 280
#define CAL_YMAX 3850

static uint16_t xpt2046_read_cmd(uint8_t cmd)
{
    uint8_t tx[3] = { cmd, 0x00, 0x00 };
    uint8_t rx[3] = { 0 };

    spi_transaction_t t = {
        .length = 24,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };

    esp_err_t err = spi_device_transmit(s_touch_spi, &t);
    if (err != ESP_OK) return 0;

    return ((rx[1] << 8) | rx[2]) >> 3;
}

bool cyd_touch_read(cyd_touch_point_t *pt)
{
    if (!pt || !s_touch_spi) return false;

    // Fast check IRQ pin (active low when touched)
    if (gpio_get_level(CYD_PIN_TOUCH_IRQ) != 0) {
        pt->pressed = false;
        return false;
    }

    uint16_t z1 = xpt2046_read_cmd(XPT_CMD_Z1);
    uint16_t z2 = xpt2046_read_cmd(XPT_CMD_Z2);
    int16_t z = z1 + 4095 - z2;

    if (z < 300) {
        pt->pressed = false;
        return false;
    }

    // Read X and Y
    uint16_t raw_x = xpt2046_read_cmd(XPT_CMD_X);
    uint16_t raw_y = xpt2046_read_cmd(XPT_CMD_Y);

    if (raw_x < CAL_XMIN) raw_x = CAL_XMIN;
    if (raw_x > CAL_XMAX) raw_x = CAL_XMAX;
    if (raw_y < CAL_YMIN) raw_y = CAL_YMIN;
    if (raw_y > CAL_YMAX) raw_y = CAL_YMAX;

    // Convert raw ADC to 320x240 landscape coordinates
    // In CYD landscape, raw Y maps to screen X (inverted to match display orientation) and raw X maps to screen Y (inverted)
    int32_t screen_x = (int32_t)(CAL_YMAX - raw_y) * 320 / (CAL_YMAX - CAL_YMIN);
    int32_t screen_y = (int32_t)(CAL_XMAX - raw_x) * 240 / (CAL_XMAX - CAL_XMIN);

    if (screen_x < 0) screen_x = 0;
    if (screen_x >= 320) screen_x = 319;
    if (screen_y < 0) screen_y = 0;
    if (screen_y >= 240) screen_y = 239;

    pt->x = (int16_t)screen_x;
    pt->y = (int16_t)screen_y;
    pt->z = z;
    pt->pressed = true;
    return true;
}

static void touch_task(void *arg)
{
    cyd_touch_point_t pt;
    bool was_pressed = false;

    while (1) {
        bool is_pressed = cyd_touch_read(&pt);
        if (is_pressed) {
            was_pressed = true;
            if (s_touch_cb) {
                s_touch_cb(&pt);
            }
        } else if (was_pressed) {
            was_pressed = false;
            pt.pressed = false;
            if (s_touch_cb) {
                s_touch_cb(&pt);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20)); // 50 Hz touch polling
    }
}

esp_err_t cyd_touch_init(cyd_touch_callback_t cb)
{
    s_touch_cb = cb;
    ESP_LOGI(TAG, "Initializing XPT2046 Touch Controller (SPI3_HOST)...");

    gpio_config_t irq_conf = {
        .pin_bit_mask = (1ULL << CYD_PIN_TOUCH_IRQ),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&irq_conf);

    spi_bus_config_t buscfg = {
        .sclk_io_num = CYD_PIN_TOUCH_CLK,
        .mosi_io_num = CYD_PIN_TOUCH_MOSI,
        .miso_io_num = CYD_PIN_TOUCH_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_DISABLED));

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 2 * 1000 * 1000, // 2 MHz SPI for XPT2046
        .mode = 0,
        .spics_io_num = CYD_PIN_TOUCH_CS,
        .queue_size = 3,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &devcfg, &s_touch_spi));

    if (cb) {
        xTaskCreatePinnedToCore(touch_task, "cyd_touch_task", 3072, NULL, 5, NULL, 1);
    }
    ESP_LOGI(TAG, "Touch initialized");
    return ESP_OK;
}

