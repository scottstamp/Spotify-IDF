#pragma once

#include <stdbool.h>
#include <esp_err.h>

typedef void (*wifi_portal_auth_success_cb_t)(void);

esp_err_t wifi_portal_start_ap(void);
esp_err_t wifi_portal_connect_sta(void);
esp_err_t wifi_portal_start_webserver(wifi_portal_auth_success_cb_t auth_cb);
void wifi_portal_get_ip_str(char *buf, size_t max_len);
bool wifi_portal_is_connected(void);

