#include "config_manager.h"
#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "config_mgr";
#define NVS_NAMESPACE "spotify_app"

static esp_err_t read_nvs_str(nvs_handle_t handle, const char *key, char *out_val, size_t max_len)
{
    size_t required_size = 0;
    esp_err_t err = nvs_get_str(handle, key, NULL, &required_size);
    if (err != ESP_OK) {
        return err;
    }
    if (required_size > max_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    return nvs_get_str(handle, key, out_val, &required_size);
}

esp_err_t config_manager_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to repartition...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t config_manager_get(spotify_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    memset(cfg, 0, sizeof(*cfg));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    read_nvs_str(handle, "wifi_ssid", cfg->wifi_ssid, sizeof(cfg->wifi_ssid));
    read_nvs_str(handle, "wifi_pass", cfg->wifi_pass, sizeof(cfg->wifi_pass));
    read_nvs_str(handle, "client_id", cfg->client_id, sizeof(cfg->client_id));
    read_nvs_str(handle, "client_secret", cfg->client_secret, sizeof(cfg->client_secret));
    read_nvs_str(handle, "refresh_token", cfg->refresh_token, sizeof(cfg->refresh_token));

    nvs_close(handle);
    return ESP_OK;
}

bool config_manager_has_wifi(void)
{
    spotify_config_t cfg;
    if (config_manager_get(&cfg) != ESP_OK) return false;
    return strlen(cfg.wifi_ssid) > 0;
}

bool config_manager_has_spotify_creds(void)
{
    spotify_config_t cfg;
    if (config_manager_get(&cfg) != ESP_OK) return false;
    return (strlen(cfg.client_id) > 0 && strlen(cfg.client_secret) > 0);
}

bool config_manager_has_refresh_token(void)
{
    spotify_config_t cfg;
    if (config_manager_get(&cfg) != ESP_OK) return false;
    return strlen(cfg.refresh_token) > 0;
}

esp_err_t config_manager_set_wifi(const char *ssid, const char *pass)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(handle, "wifi_ssid", ssid ? ssid : "");
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "wifi_pass", pass ? pass : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t config_manager_set_spotify_creds(const char *client_id, const char *client_secret)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(handle, "client_id", client_id ? client_id : "");
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "client_secret", client_secret ? client_secret : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t config_manager_set_refresh_token(const char *token)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(handle, "refresh_token", token ? token : "");
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t config_manager_clear_refresh_token(void)
{
    return config_manager_set_refresh_token("");
}

esp_err_t config_manager_erase_all(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

