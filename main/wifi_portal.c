#include "wifi_portal.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "config_manager.h"
#include "spotify_service.h"

static const char *TAG = "wifi_portal";

static httpd_handle_t s_server = NULL;
static bool s_connected = false;
static char s_ip_str[32] = "192.168.4.1";
static wifi_portal_auth_success_cb_t s_auth_cb = NULL;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        ESP_LOGI(TAG, "WiFi disconnected, retrying...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
        ESP_LOGI(TAG, "WiFi Connected! IP Address: %s", s_ip_str);
    }
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    spotify_config_t cfg;
    config_manager_get(&cfg);

    char html[2048];
    if (!s_connected) {
        // SoftAP Setup Page
        snprintf(html, sizeof(html),
            "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<style>body{font-family:sans-serif;background:#121212;color:#fff;padding:20px;max-width:400px;margin:auto}"
            "input{width:100%%;padding:10px;margin:8px 0;box-sizing:border-box;border-radius:4px;border:1px solid #333}"
            "button{background:#1DB954;color:#fff;border:none;padding:12px;width:100%%;border-radius:20px;font-size:16px;cursor:pointer}</style></head>"
            "<body><h2>CYD Spotify Setup</h2>"
            "<form action='/save' method='POST'>"
            "<label>WiFi SSID:</label><input name='ssid' value='%s'>"
            "<label>WiFi Password:</label><input type='password' name='pass'>"
            "<label>Spotify Client ID:</label><input name='cid' value='%s'>"
            "<label>Spotify Client Secret:</label><input type='password' name='csec' value='%s'>"
            "<button type='submit'>Save & Connect</button>"
            "</form></body></html>",
            cfg.wifi_ssid, cfg.client_id, cfg.client_secret);
    } else {
        // OAuth Authorization Link Page
        char redirect_uri[128];
        snprintf(redirect_uri, sizeof(redirect_uri), "https%%3A%%2F%%2F%s%%2Fcallback%%2F", s_ip_str);

        snprintf(html, sizeof(html),
            "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<style>body{font-family:sans-serif;background:#121212;color:#fff;padding:20px;text-align:center;max-width:400px;margin:auto}"
            "a{background:#1DB954;color:#fff;text-decoration:none;display:block;padding:14px;border-radius:24px;font-size:18px;margin:20px 0}"
            "code{background:#222;padding:4px 8px;border-radius:4px;display:block;word-break:break-all;margin:10px 0}</style></head>"
            "<body><h2>Connect to Spotify</h2>"
            "<p>1. Ensure this exact Redirect URI is added in your Spotify Developer Dashboard:</p>"
            "<code>https://%s/callback/</code>"
            "<p>2. Tap the button below to authorize:</p>"
            "<a href='https://accounts.spotify.com/authorize?client_id=%s&response_type=code&redirect_uri=%s&scope=user-read-playback-state%%20user-modify-playback-state'>"
            "Log In with Spotify</a></body></html>",
            s_ip_str, cfg.client_id, redirect_uri);
    }

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t callback_get_handler(httpd_req_t *req)
{
    char query[512] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    char code[384] = {0};
    if (httpd_query_key_value(query, "code", code, sizeof(code)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing code query param");
        return ESP_FAIL;
    }

    char redirect_uri[128];
    snprintf(redirect_uri, sizeof(redirect_uri), "https://%s/callback/", s_ip_str);

    bool ok = spotify_service_exchange_code(code, redirect_uri);
    if (ok) {
        const char *resp = "<!DOCTYPE html><html><body style='font-family:sans-serif;background:#121212;color:#fff;text-align:center;padding:50px'>"
                           "<h2 style='color:#1DB954'>Success!</h2><p>Your desk controller is authenticated and starting up.</p></body></html>";
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
        if (s_auth_cb) {
            s_auth_cb();
        }
        return ESP_OK;
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to exchange token with Spotify");
        return ESP_FAIL;
    }
}

static void url_decode(char *dst, const char *src)
{
    char a, b;
    while (*src) {
        if ((*src == '%') &&
            ((a = src[1]) && (b = src[2])) &&
            (isxdigit(a) && isxdigit(b))) {
            if (a >= 'a') a -= 'a' - 'A';
            if (a >= 'A') a -= ('A' - 10);
            else a -= '0';
            if (b >= 'a') b -= 'a' - 'A';
            if (b >= 'A') b -= ('A' - 10);
            else b -= '0';
            *dst++ = 16 * a + b;
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    char buf[512] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;

    char raw_val[128], decoded_val[128];

    if (httpd_query_key_value(buf, "ssid", raw_val, sizeof(raw_val)) == ESP_OK) {
        url_decode(decoded_val, raw_val);
        char pass[128] = {0};
        if (httpd_query_key_value(buf, "pass", raw_val, sizeof(raw_val)) == ESP_OK) {
            url_decode(pass, raw_val);
        }
        config_manager_set_wifi(decoded_val, pass);
    }

    if (httpd_query_key_value(buf, "cid", raw_val, sizeof(raw_val)) == ESP_OK) {
        url_decode(decoded_val, raw_val);
        char csec[128] = {0};
        if (httpd_query_key_value(buf, "csec", raw_val, sizeof(raw_val)) == ESP_OK) {
            url_decode(csec, raw_val);
        }
        config_manager_set_spotify_creds(decoded_val, csec);
    }

    const char *resp = "Settings saved! Connecting to WiFi...";
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

esp_err_t wifi_portal_start_webserver(wifi_portal_auth_success_cb_t auth_cb)
{
    s_auth_cb = auth_cb;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.stack_size = 16384;

    ESP_LOGI(TAG, "Starting web server on port %d", config.server_port);
    if (httpd_start(&s_server, &config) != ESP_OK) {
        return ESP_FAIL;
    }

    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    httpd_register_uri_handler(s_server, &root_uri);

    httpd_uri_t callback_uri = {
        .uri = "/callback/",
        .method = HTTP_GET,
        .handler = callback_get_handler,
    };
    httpd_register_uri_handler(s_server, &callback_uri);

    httpd_uri_t save_uri = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = save_post_handler,
    };
    httpd_register_uri_handler(s_server, &save_uri);

    return ESP_OK;
}

esp_err_t wifi_portal_start_ap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "CYD-Spotify-Setup",
            .ssid_len = strlen("CYD-Spotify-Setup"),
            .password = "",
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    strcpy(s_ip_str, "192.168.4.1");
    ESP_LOGI(TAG, "SoftAP started: SSID=CYD-Spotify-Setup, IP=192.168.4.1");
    return ESP_OK;
}

esp_err_t wifi_portal_connect_sta(void)
{
    spotify_config_t cfg;
    if (config_manager_get(&cfg) != ESP_OK) return ESP_FAIL;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t sta_config = {0};
    strlcpy((char *)sta_config.sta.ssid, cfg.wifi_ssid, sizeof(sta_config.sta.ssid));
    strlcpy((char *)sta_config.sta.password, cfg.wifi_pass, sizeof(sta_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to WiFi: %s...", cfg.wifi_ssid);

    // Wait up to 10 seconds for connection
    int attempts = 0;
    while (!s_connected && attempts++ < 20) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    return s_connected ? ESP_OK : ESP_FAIL;
}

void wifi_portal_get_ip_str(char *buf, size_t max_len)
{
    if (buf && max_len > 0) {
        strncpy(buf, s_ip_str, max_len - 1);
        buf[max_len - 1] = '\0';
    }
}

bool wifi_portal_is_connected(void)
{
    return s_connected;
}

