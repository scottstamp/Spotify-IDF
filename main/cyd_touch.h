#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>

#define CYD_PIN_TOUCH_CLK   25
#define CYD_PIN_TOUCH_MOSI  32
#define CYD_PIN_TOUCH_MISO  39
#define CYD_PIN_TOUCH_CS    33
#define CYD_PIN_TOUCH_IRQ   36

typedef struct {
    int16_t x;
    int16_t y;
    int16_t z;
    bool pressed;
} cyd_touch_point_t;

typedef void (*cyd_touch_callback_t)(const cyd_touch_point_t *pt);

esp_err_t cyd_touch_init(cyd_touch_callback_t cb);
bool cyd_touch_read(cyd_touch_point_t *pt);

