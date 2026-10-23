#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define SPOTIFY_IMAGE_SIZE 150

typedef struct {
    bool is_playing;
    bool has_track;
    bool image_loaded;
    bool image_updated;
    uint32_t progress_ms;
    uint32_t duration_ms;
    char track_name[128];
    char artist_name[128];
    char album_name[128];
    char image_url[256];
} spotify_playback_state_t;

typedef enum {
    SPOTIFY_CMD_PLAY_PAUSE,
    SPOTIFY_CMD_NEXT,
    SPOTIFY_CMD_PREV,
    SPOTIFY_CMD_REFRESH_NOW
} spotify_user_cmd_t;

typedef void (*spotify_state_callback_t)(const spotify_playback_state_t *state);

esp_err_t spotify_service_init(spotify_state_callback_t cb);
void spotify_service_send_cmd(spotify_user_cmd_t cmd);
bool spotify_service_exchange_code(const char *code, const char *redirect_uri);
