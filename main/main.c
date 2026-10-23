#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config_manager.h"
#include "cyd_display.h"
#include "ui_view.h"
#include "wifi_portal.h"
#include "spotify_service.h"

static const char *TAG = "main";
#define BOOT_BUTTON_PIN GPIO_NUM_0

static void on_auth_success(void)
{
    ESP_LOGI(TAG, "OAuth Authentication succeeded! Launching Player...");
    ui_view_set_screen(UI_SCREEN_PLAYER, NULL);
    spotify_service_init(ui_view_update_state);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting CYD Spotify Desk Controller (ESP-IDF v6.0.2)...");

    // 1. Initialize NVS
    ESP_ERROR_CHECK(config_manager_init());

    // 2. Initialize Display and UI Engine (Runs on Core 1)
    ESP_ERROR_CHECK(cyd_display_init());
    ui_view_init();

    // 3. Check for BOOT button hold to force config mode
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&btn_cfg);

    bool force_config = (gpio_get_level(BOOT_BUTTON_PIN) == 0);
    if (force_config) {
        ESP_LOGW(TAG, "BOOT button pressed at startup. Forcing configuration portal...");
        config_manager_clear_refresh_token();
    }

    bool has_wifi = config_manager_has_wifi();
    bool has_spotify_creds = config_manager_has_spotify_creds();
    bool has_refresh_token = config_manager_has_refresh_token();

    if (!has_wifi || !has_spotify_creds) {
        // Step 1: Provisioning Mode (SoftAP)
        ESP_LOGI(TAG, "WiFi or Spotify credentials missing. Launching Setup Portal...");
        ui_view_set_screen(UI_SCREEN_SETUP, "Connect WiFi: CYD-Spotify-Setup\nOpen: http://192.168.4.1");
        wifi_portal_start_ap();
        wifi_portal_start_webserver(on_auth_success);
        return;
    }

    // Step 2: Connect to configured WiFi Station
    ui_view_set_screen(UI_SCREEN_SETUP, "Connecting to WiFi...");
    esp_err_t err = wifi_portal_connect_sta();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to connect to WiFi! Launching SoftAP setup...");
        ui_view_set_screen(UI_SCREEN_SETUP, "WiFi Connection Failed!\nOpening setup portal...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        wifi_portal_start_ap();
        wifi_portal_start_webserver(on_auth_success);
        return;
    }

    char ip_str[32];
    wifi_portal_get_ip_str(ip_str, sizeof(ip_str));

    // Step 3: Check Spotify OAuth Refresh Token
    if (!has_refresh_token || force_config) {
        ESP_LOGI(TAG, "No Spotify refresh token. Starting OAuth server...");
        ui_view_set_screen(UI_SCREEN_AUTH_PENDING, ip_str);
        wifi_portal_start_webserver(on_auth_success);
    } else {
        // Step 4: Fully configured! Start playback display & Core 0 worker task
        ESP_LOGI(TAG, "Device configured! Starting Spotify service...");
        ui_view_set_screen(UI_SCREEN_PLAYER, NULL);
        wifi_portal_start_webserver(on_auth_success); // Keep server alive for status/reauth
        spotify_service_init(ui_view_update_state);
    }
}

