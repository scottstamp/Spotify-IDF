#include "ui_view.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "cyd_display.h"
#include "cyd_touch.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "ui_view";

static ui_screen_t s_current_screen = UI_SCREEN_SETUP;
static spotify_playback_state_t s_ui_state = {0};
static SemaphoreHandle_t s_lvgl_mux = NULL;

static int64_t s_last_sync_us = 0;
static uint32_t s_interpolated_progress_ms = 0;

// Single shared 150x150 RGB565 album art buffer
static uint16_t s_album_art[SPOTIFY_IMAGE_SIZE * SPOTIFY_IMAGE_SIZE] = {0};
static lv_image_dsc_t s_album_img_dsc = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_RGB565_SWAPPED,
        .flags = 0,
        .w = SPOTIFY_IMAGE_SIZE,
        .h = SPOTIFY_IMAGE_SIZE,
        .stride = SPOTIFY_IMAGE_SIZE * sizeof(uint16_t),
    },
    .data_size = SPOTIFY_IMAGE_SIZE * SPOTIFY_IMAGE_SIZE * sizeof(uint16_t),
    .data = (const uint8_t *)s_album_art,
};

// LVGL Screen Objects
static lv_obj_t *s_screen_player = NULL;
static lv_obj_t *s_screen_setup = NULL;
static lv_obj_t *s_screen_auth = NULL;

// Player Screen Widgets
static lv_obj_t *s_img_album = NULL;
static lv_obj_t *s_album_placeholder = NULL;
static lv_obj_t *s_overlay_pause = NULL;
static lv_obj_t *s_lbl_pause_icon = NULL;
static lv_obj_t *s_lbl_track = NULL;
static lv_obj_t *s_lbl_artist = NULL;
static lv_obj_t *s_bar_progress = NULL;
static lv_obj_t *s_lbl_time_cur = NULL;
static lv_obj_t *s_lbl_time_dur = NULL;

// Setup & Auth Widgets
static lv_obj_t *s_lbl_setup_msg = NULL;
static lv_obj_t *s_lbl_auth_url = NULL;

uint16_t *ui_view_get_image_buffer(void)
{
    return s_album_art;
}

static void format_time(uint32_t ms, char *buf, size_t max_len)
{
    uint32_t sec = ms / 1000;
    uint32_t min = sec / 60;
    sec %= 60;
    snprintf(buf, max_len, "%lu:%02lu", (unsigned long)min, (unsigned long)sec);
}

// Button click callbacks
static void prev_btn_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "Touch: PREVIOUS");
    spotify_service_send_cmd(SPOTIFY_CMD_PREV);
}

static void play_btn_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "Touch: PLAY/PAUSE");
    spotify_service_send_cmd(SPOTIFY_CMD_PLAY_PAUSE);
}

static void next_btn_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "Touch: NEXT");
    spotify_service_send_cmd(SPOTIFY_CMD_NEXT);
}

// Display flush callback for LVGL 9
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)lv_display_get_user_data(disp);
    int x1 = area->x1;
    int y1 = area->y1;
    int x2 = area->x2;
    int y2 = area->y2;

    esp_lcd_panel_draw_bitmap(panel_handle, x1, y1, x2 + 1, y2 + 1, px_map);
}

// Touch input callback for LVGL 9
static void lvgl_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    cyd_touch_point_t pt;
    if (cyd_touch_read(&pt)) {
        data->point.x = pt.x;
        data->point.y = pt.y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void create_player_screen(void)
{
    s_screen_player = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen_player, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_screen_player, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_screen_player, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(s_screen_player, LV_OBJ_FLAG_SCROLLABLE);

    // 1. Centered Album Art Image (170x170 px: X=75, Y=4)
    s_img_album = lv_image_create(s_screen_player);
    lv_obj_set_pos(s_img_album, 75, 4);
    lv_obj_set_size(s_img_album, 170, 170);
    lv_obj_set_style_radius(s_img_album, 10, 0);
    lv_obj_set_style_clip_corner(s_img_album, true, 0);
    lv_image_set_scale(s_img_album, 290); // Scale 150px buffer to 170px (170*256/150 = 290)
    lv_image_set_src(s_img_album, &s_album_img_dsc);
    lv_obj_add_flag(s_img_album, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_img_album, play_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_hidden(s_img_album, true);

    // Album Placeholder (when no artwork is loaded)
    s_album_placeholder = lv_obj_create(s_screen_player);
    lv_obj_set_pos(s_album_placeholder, 75, 4);
    lv_obj_set_size(s_album_placeholder, 170, 170);
    lv_obj_set_style_radius(s_album_placeholder, 10, 0);
    lv_obj_set_style_bg_color(s_album_placeholder, lv_color_hex(0x242424), 0);
    lv_obj_set_style_border_width(s_album_placeholder, 1, 0);
    lv_obj_set_style_border_color(s_album_placeholder, lv_color_hex(0x383838), 0);
    lv_obj_set_style_shadow_width(s_album_placeholder, 0, 0);
    lv_obj_set_scrollbar_mode(s_album_placeholder, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(s_album_placeholder, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_album_placeholder, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_album_placeholder, play_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *placeholder_lbl = lv_label_create(s_album_placeholder);
    lv_label_set_text(placeholder_lbl, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(placeholder_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(placeholder_lbl, lv_color_hex(0x1DB954), 0);
    lv_obj_center(placeholder_lbl);

    // Pause Overlay Badge on top of Album Art
    s_overlay_pause = lv_obj_create(s_screen_player);
    lv_obj_set_pos(s_overlay_pause, 132, 61);
    lv_obj_set_size(s_overlay_pause, 56, 56);
    lv_obj_set_style_radius(s_overlay_pause, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_overlay_pause, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_overlay_pause, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_overlay_pause, 2, 0);
    lv_obj_set_style_border_color(s_overlay_pause, lv_color_hex(0x1DB954), 0);
    lv_obj_set_style_shadow_width(s_overlay_pause, 0, 0);
    lv_obj_set_scrollbar_mode(s_overlay_pause, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(s_overlay_pause, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_overlay_pause, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_overlay_pause, play_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_hidden(s_overlay_pause, true);

    s_lbl_pause_icon = lv_label_create(s_overlay_pause);
    lv_label_set_text(s_lbl_pause_icon, LV_SYMBOL_PAUSE);
    lv_obj_set_style_text_font(s_lbl_pause_icon, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_lbl_pause_icon, lv_color_hex(0x1DB954), 0);
    lv_obj_center(s_lbl_pause_icon);

    // 2. Flanking Prev / Next Buttons (Flat without drop shadow)
    // Previous Button (Left of Album Art: X=15, Y=67, W=44, H=44)
    lv_obj_t *btn_prev = lv_button_create(s_screen_player);
    lv_obj_set_pos(btn_prev, 15, 67);
    lv_obj_set_size(btn_prev, 44, 44);
    lv_obj_set_style_radius(btn_prev, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn_prev, lv_color_hex(0x222222), 0);
    lv_obj_set_style_bg_color(btn_prev, lv_color_hex(0x383838), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn_prev, 1, 0);
    lv_obj_set_style_border_color(btn_prev, lv_color_hex(0x333333), 0);
    lv_obj_set_style_shadow_width(btn_prev, 0, 0);
    lv_obj_set_style_shadow_width(btn_prev, 0, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_spread(btn_prev, 0, 0);
    lv_obj_set_style_shadow_opa(btn_prev, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(btn_prev, prev_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_prev = lv_label_create(btn_prev);
    lv_label_set_text(lbl_prev, LV_SYMBOL_PREV);
    lv_obj_set_style_text_font(lbl_prev, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_prev, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(lbl_prev);

    // Next Button (Right of Album Art: X=261, Y=67, W=44, H=44)
    lv_obj_t *btn_next = lv_button_create(s_screen_player);
    lv_obj_set_pos(btn_next, 261, 67);
    lv_obj_set_size(btn_next, 44, 44);
    lv_obj_set_style_radius(btn_next, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn_next, lv_color_hex(0x222222), 0);
    lv_obj_set_style_bg_color(btn_next, lv_color_hex(0x383838), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn_next, 1, 0);
    lv_obj_set_style_border_color(btn_next, lv_color_hex(0x333333), 0);
    lv_obj_set_style_shadow_width(btn_next, 0, 0);
    lv_obj_set_style_shadow_width(btn_next, 0, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_spread(btn_next, 0, 0);
    lv_obj_set_style_shadow_opa(btn_next, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(btn_next, next_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_next = lv_label_create(btn_next);
    lv_label_set_text(lbl_next, LV_SYMBOL_NEXT);
    lv_obj_set_style_text_font(lbl_next, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_next, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(lbl_next);

    // 3. Track Metadata at Bottom (Above Progress Bar)
    // Song Title (Montserrat 12: X=10, Y=180, W=300)
    s_lbl_track = lv_label_create(s_screen_player);
    lv_obj_set_pos(s_lbl_track, 10, 180);
    lv_obj_set_width(s_lbl_track, 300);
    lv_obj_set_style_text_align(s_lbl_track, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_lbl_track, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_style_text_font(s_lbl_track, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_lbl_track, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(s_lbl_track, "Not Playing");

    // Artist Name (Montserrat 12: X=10, Y=198, W=300)
    s_lbl_artist = lv_label_create(s_screen_player);
    lv_obj_set_pos(s_lbl_artist, 10, 198);
    lv_obj_set_width(s_lbl_artist, 300);
    lv_obj_set_style_text_align(s_lbl_artist, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_lbl_artist, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_style_text_font(s_lbl_artist, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_lbl_artist, lv_color_hex(0x1DB954), 0);
    lv_label_set_text(s_lbl_artist, "Connect a Device");

    // 4. Progress Bar (X=14, Y=217, W=292, H=5)
    s_bar_progress = lv_bar_create(s_screen_player);
    lv_obj_set_pos(s_bar_progress, 14, 217);
    lv_obj_set_size(s_bar_progress, 292, 5);
    lv_obj_set_style_radius(s_bar_progress, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_radius(s_bar_progress, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_bar_progress, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_color(s_bar_progress, lv_color_hex(0x1DB954), LV_PART_INDICATOR);
    lv_bar_set_range(s_bar_progress, 0, 100);
    lv_bar_set_value(s_bar_progress, 0, LV_ANIM_OFF);

    // Elapsed Time (Montserrat 10: X=14, Y=225)
    s_lbl_time_cur = lv_label_create(s_screen_player);
    lv_obj_set_pos(s_lbl_time_cur, 14, 225);
    lv_obj_set_style_text_font(s_lbl_time_cur, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_lbl_time_cur, lv_color_hex(0x888888), 0);
    lv_label_set_text(s_lbl_time_cur, "0:00");

    // Total Duration (Montserrat 10: X=256, Y=225, W=50, right-aligned to match bar edge at 306)
    s_lbl_time_dur = lv_label_create(s_screen_player);
    lv_obj_set_pos(s_lbl_time_dur, 256, 225);
    lv_obj_set_size(s_lbl_time_dur, 50, 14);
    lv_obj_set_style_text_align(s_lbl_time_dur, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(s_lbl_time_dur, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_lbl_time_dur, lv_color_hex(0x888888), 0);
    lv_label_set_text(s_lbl_time_dur, "0:00");
}

static void create_setup_screen(void)
{
    s_screen_setup = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen_setup, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_screen_setup, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_screen_setup, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(s_screen_setup, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_screen_setup);
    lv_obj_set_pos(title, 20, 25);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x1DB954), 0);
    lv_label_set_text(title, "CYD Spotify Setup");

    lv_obj_t *line = lv_obj_create(s_screen_setup);
    lv_obj_set_pos(line, 20, 52);
    lv_obj_set_size(line, 280, 2);
    lv_obj_set_style_bg_color(line, lv_color_hex(0x1DB954), 0);
    lv_obj_set_style_border_width(line, 0, 0);

    lv_obj_t *card = lv_obj_create(s_screen_setup);
    lv_obj_set_pos(card, 20, 70);
    lv_obj_set_size(card, 280, 140);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x181818), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x333333), 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_setup_msg = lv_label_create(card);
    lv_obj_set_width(s_lbl_setup_msg, 240);
    lv_label_set_long_mode(s_lbl_setup_msg, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(s_lbl_setup_msg, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_setup_msg, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(s_lbl_setup_msg, "Initializing system...");
    lv_obj_center(s_lbl_setup_msg);
}

static void create_auth_screen(void)
{
    s_screen_auth = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen_auth, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_screen_auth, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_screen_auth, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(s_screen_auth, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_screen_auth);
    lv_obj_set_pos(title, 20, 20);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x1DB954), 0);
    lv_label_set_text(title, "Spotify Login");

    lv_obj_t *line = lv_obj_create(s_screen_auth);
    lv_obj_set_pos(line, 20, 46);
    lv_obj_set_size(line, 280, 2);
    lv_obj_set_style_bg_color(line, lv_color_hex(0x1DB954), 0);
    lv_obj_set_style_border_width(line, 0, 0);

    lv_obj_t *hint1 = lv_label_create(s_screen_auth);
    lv_obj_set_pos(hint1, 20, 60);
    lv_obj_set_style_text_font(hint1, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint1, lv_color_hex(0xCCCCCC), 0);
    lv_label_set_text(hint1, "Open browser on your phone/PC:");

    // Highlighted URL Badge
    lv_obj_t *badge = lv_obj_create(s_screen_auth);
    lv_obj_set_pos(badge, 20, 90);
    lv_obj_set_size(badge, 280, 42);
    lv_obj_set_style_radius(badge, 8, 0);
    lv_obj_set_style_bg_color(badge, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_border_color(badge, lv_color_hex(0x1DB954), 0);
    lv_obj_set_scrollbar_mode(badge, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(badge, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_auth_url = lv_label_create(badge);
    lv_obj_set_style_text_font(s_lbl_auth_url, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_lbl_auth_url, lv_color_hex(0x1DB954), 0);
    lv_label_set_text(s_lbl_auth_url, "http://192.168.4.1/");
    lv_obj_center(s_lbl_auth_url);

    lv_obj_t *hint2 = lv_label_create(s_screen_auth);
    lv_obj_set_pos(hint2, 20, 145);
    lv_obj_set_width(hint2, 280);
    lv_label_set_long_mode(hint2, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(hint2, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hint2, lv_color_hex(0x888888), 0);
    lv_label_set_text(hint2, "Click 'Log In with Spotify' on the webpage to link your account.");
}

void ui_view_set_screen(ui_screen_t screen, const char *msg)
{
    if (!s_lvgl_mux) return;
    xSemaphoreTake(s_lvgl_mux, portMAX_DELAY);
    s_current_screen = screen;

    if (screen == UI_SCREEN_SETUP) {
        if (msg && s_lbl_setup_msg) {
            lv_label_set_text(s_lbl_setup_msg, msg);
        }
        lv_screen_load(s_screen_setup);
    } else if (screen == UI_SCREEN_AUTH_PENDING) {
        if (msg && s_lbl_auth_url) {
            char buf[64];
            snprintf(buf, sizeof(buf), "http://%s/", msg);
            lv_label_set_text(s_lbl_auth_url, buf);
        }
        lv_screen_load(s_screen_auth);
    } else if (screen == UI_SCREEN_PLAYER) {
        lv_screen_load(s_screen_player);
    }
    xSemaphoreGive(s_lvgl_mux);
}

void ui_view_update_state(const spotify_playback_state_t *state)
{
    if (!s_lvgl_mux) return;
    xSemaphoreTake(s_lvgl_mux, portMAX_DELAY);
    s_ui_state = *state;
    s_last_sync_us = esp_timer_get_time();
    s_interpolated_progress_ms = state->progress_ms;

    if (s_current_screen == UI_SCREEN_PLAYER) {
        if (state->has_track) {
            lv_label_set_text(s_lbl_track, state->track_name);
            lv_label_set_text(s_lbl_artist, state->artist_name);

            if (state->image_updated) {
                lv_image_set_src(s_img_album, &s_album_img_dsc);
                lv_obj_set_hidden(s_img_album, false);
                lv_obj_set_hidden(s_album_placeholder, true);
            } else if (!state->image_loaded) {
                lv_obj_set_hidden(s_img_album, true);
                lv_obj_set_hidden(s_album_placeholder, false);
            }

            // Pause overlay: visible when paused, hidden when playing
            lv_obj_set_hidden(s_overlay_pause, state->is_playing);

            if (state->duration_ms > 0) {
                lv_bar_set_range(s_bar_progress, 0, state->duration_ms);
                lv_bar_set_value(s_bar_progress, state->progress_ms, LV_ANIM_OFF);

                char buf[16];
                format_time(state->progress_ms, buf, sizeof(buf));
                lv_label_set_text(s_lbl_time_cur, buf);

                format_time(state->duration_ms, buf, sizeof(buf));
                lv_label_set_text(s_lbl_time_dur, buf);
            }
        } else {
            lv_label_set_text(s_lbl_track, "Not Playing");
            lv_label_set_text(s_lbl_artist, "Connect a Device");
            lv_obj_set_hidden(s_img_album, true);
            lv_obj_set_hidden(s_album_placeholder, false);
            lv_obj_set_hidden(s_overlay_pause, true);
            lv_bar_set_value(s_bar_progress, 0, LV_ANIM_OFF);
            lv_label_set_text(s_lbl_time_cur, "0:00");
            lv_label_set_text(s_lbl_time_dur, "0:00");
        }
    }
    xSemaphoreGive(s_lvgl_mux);
}

static void ui_render_task(void *arg)
{
    ESP_LOGI(TAG, "LVGL 9 Render Task started on Core 1");

    uint32_t last_tick = esp_timer_get_time() / 1000;

    while (1) {
        uint32_t now = esp_timer_get_time() / 1000;
        lv_tick_inc(now - last_tick);
        last_tick = now;

        xSemaphoreTake(s_lvgl_mux, portMAX_DELAY);

        // Smooth progress bar advancement during playback
        if (s_current_screen == UI_SCREEN_PLAYER && s_ui_state.has_track &&
            s_ui_state.is_playing && s_ui_state.duration_ms > 0) {
            int64_t elapsed_us = esp_timer_get_time() - s_last_sync_us;
            uint32_t current_prog = s_interpolated_progress_ms + (uint32_t)(elapsed_us / 1000);
            if (current_prog > s_ui_state.duration_ms) current_prog = s_ui_state.duration_ms;

            lv_bar_set_value(s_bar_progress, current_prog, LV_ANIM_OFF);

            char buf[16];
            format_time(current_prog, buf, sizeof(buf));
            lv_label_set_text(s_lbl_time_cur, buf);
        }

        uint32_t time_till_next = lv_timer_handler();
        xSemaphoreGive(s_lvgl_mux);

        if (time_till_next < 10) time_till_next = 10;
        if (time_till_next > 30) time_till_next = 30;

        vTaskDelay(pdMS_TO_TICKS(time_till_next));
    }
}

void ui_view_init(void)
{
    s_lvgl_mux = xSemaphoreCreateMutex();

    ESP_LOGI(TAG, "Initializing LVGL 9 Core...");
    lv_init();

    // Allocate DMA-capable draw buffers (10 lines each)
    size_t draw_buf_size = CYD_LCD_WIDTH * 10 * sizeof(uint16_t);
    void *buf1 = heap_caps_malloc(draw_buf_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void *buf2 = heap_caps_malloc(draw_buf_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    assert(buf1 != NULL);

    // Create LVGL 9 display
    lv_display_t *disp = lv_display_create(CYD_LCD_WIDTH, CYD_LCD_HEIGHT);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_buffers(disp, buf1, buf2, draw_buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, lvgl_flush_cb);
    lv_display_set_user_data(disp, cyd_display_get_panel_handle());
    cyd_display_set_lvgl_disp(disp);

    // Initialize hardware touch & LVGL 9 input device
    cyd_touch_init(NULL);
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, lvgl_touch_read_cb);

    // Build modern Spotify UI screens
    create_player_screen();
    create_setup_screen();
    create_auth_screen();

    lv_screen_load(s_screen_setup);

    // Core 1 UI Task (runs LVGL 9 timers, animation, and touch handling at 50Hz)
    xTaskCreatePinnedToCore(ui_render_task, "ui_render_task", 6144, NULL, 5, NULL, 1);
}
