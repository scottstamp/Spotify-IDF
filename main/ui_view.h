#pragma once

#include "spotify_service.h"

typedef enum {
    UI_SCREEN_SETUP,
    UI_SCREEN_AUTH_PENDING,
    UI_SCREEN_PLAYER
} ui_screen_t;

void ui_view_init(void);
void ui_view_set_screen(ui_screen_t screen, const char *msg);
void ui_view_update_state(const spotify_playback_state_t *state);
uint16_t *ui_view_get_image_buffer(void);
