#pragma once

#include <stdbool.h>
#include <esp_err.h>

#define CONFIG_MAX_STR_LEN 128
#define CONFIG_TOKEN_STR_LEN 512

typedef struct {
    char wifi_ssid[CONFIG_MAX_STR_LEN];
    char wifi_pass[CONFIG_MAX_STR_LEN];
    char client_id[CONFIG_MAX_STR_LEN];
    char client_secret[CONFIG_MAX_STR_LEN];
    char refresh_token[CONFIG_TOKEN_STR_LEN];
} spotify_config_t;

esp_err_t config_manager_init(void);
bool config_manager_has_wifi(void);
bool config_manager_has_spotify_creds(void);
bool config_manager_has_refresh_token(void);

esp_err_t config_manager_get(spotify_config_t *cfg);
esp_err_t config_manager_set_wifi(const char *ssid, const char *pass);
esp_err_t config_manager_set_spotify_creds(const char *client_id, const char *client_secret);
esp_err_t config_manager_set_refresh_token(const char *token);
esp_err_t config_manager_clear_refresh_token(void);
esp_err_t config_manager_erase_all(void);

