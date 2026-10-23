#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

#define CYD_LCD_WIDTH   320
#define CYD_LCD_HEIGHT  240

#define CYD_PIN_LCD_MISO 12
#define CYD_PIN_LCD_MOSI 13
#define CYD_PIN_LCD_SCLK 14
#define CYD_PIN_LCD_CS   15
#define CYD_PIN_LCD_DC   2
#define CYD_PIN_LCD_RST  -1
#define CYD_PIN_LCD_BL   21

esp_err_t cyd_display_init(void);
void cyd_display_set_backlight(uint8_t brightness_pct);
esp_lcd_panel_handle_t cyd_display_get_panel_handle(void);
void cyd_display_set_lvgl_disp(lv_display_t *disp);
