#include "spotify_service.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "config_manager.h"
#include "ui_view.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "spotify_svc";

static char s_access_token[512] = {0};
static int64_t s_token_expires_at_us = 0;
static QueueHandle_t s_cmd_queue = NULL;
static spotify_state_callback_t s_state_cb = NULL;
static spotify_playback_state_t s_current_state = {0};
static int64_t s_last_poll_time_us = 0;
static char s_last_track_id[64] = {0};

typedef struct {
    char *buf;
    size_t len;
    size_t capacity;
} http_response_buffer_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_response_buffer_t *resp = (http_response_buffer_t *)evt->user_data;
    if (!resp) return ESP_OK;

    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (resp->len + evt->data_len < resp->capacity) {
            memcpy(resp->buf + resp->len, evt->data, evt->data_len);
            resp->len += evt->data_len;
            resp->buf[resp->len] = '\0';
        }
    }
    return ESP_OK;
}

#include "esp32/rom/tjpgd.h"

typedef struct {
    esp_http_client_handle_t client;
    uint16_t *out_buf;
    int target_w;
    int target_h;
    int offset_x;
    int offset_y;
} jpeg_http_stream_ctx_t;

static UINT http_jpeg_feed(JDEC *jd, BYTE *buff, UINT nbyte)
{
    jpeg_http_stream_ctx_t *ctx = (jpeg_http_stream_ctx_t *)jd->device;
    if (buff) {
        UINT total = 0;
        while (total < nbyte) {
            int r = esp_http_client_read(ctx->client, (char *)buff + total, nbyte - total);
            if (r <= 0) break;
            total += r;
        }
        return total;
    } else {
        char dummy[128];
        UINT discarded = 0;
        while (discarded < nbyte) {
            UINT to_read = (nbyte - discarded > sizeof(dummy)) ? sizeof(dummy) : (nbyte - discarded);
            int r = esp_http_client_read(ctx->client, dummy, to_read);
            if (r <= 0) break;
            discarded += r;
        }
        return discarded;
    }
}

static UINT jpeg_stream_out_cb(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpeg_http_stream_ctx_t *ctx = (jpeg_http_stream_ctx_t *)jd->device;
    uint8_t *in = (uint8_t *)bitmap;
    uint16_t *out_buf = ctx->out_buf;
    int target_w = ctx->target_w;
    int target_h = ctx->target_h;
    int off_x = ctx->offset_x;
    int off_y = ctx->offset_y;

    for (int y = rect->top; y <= rect->bottom; y++) {
        int dst_y = y + off_y;
        for (int x = rect->left; x <= rect->right; x++) {
            int dst_x = x + off_x;
            if (dst_x >= 0 && dst_x < target_w && dst_y >= 0 && dst_y < target_h) {
                // JD_FORMAT == 0 is RGB888: in[0]=R, in[1]=G, in[2]=B
                uint16_t color = ((in[0] & 0xF8) << 8) | ((in[1] & 0xFC) << 3) | (in[2] >> 3);
                // Big-endian byte swap matching LV_COLOR_FORMAT_RGB565_SWAPPED
                uint16_t swapped = (color >> 8) | (color << 8);
                out_buf[dst_y * target_w + dst_x] = swapped;
            }
            in += 3;
        }
    }
    return 1; // 1 = continue decoding
}

static bool download_album_art(const char *url, uint16_t *out_buf)
{
    if (!url || strlen(url) == 0) return false;
    ESP_LOGI(TAG, "Streaming album art on-the-fly: %s", url);

    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 8000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client for album art");
        return false;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %d", err);
        esp_http_client_cleanup(client);
        return false;
    }

    int64_t content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);

    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP fetch failed: status=%d", status_code);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    // Allocate 3.5 KB scratchpad for the ROM decompressor (zero image memory buffer!)
    size_t workbuf_size = 3500;
    void *workbuf = heap_caps_malloc(workbuf_size, MALLOC_CAP_8BIT);
    if (!workbuf) {
        ESP_LOGE(TAG, "Failed to allocate 3.5KB tjpgd workbuf!");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    // Clear output buffer with black before streaming
    memset(out_buf, 0, SPOTIFY_IMAGE_SIZE * SPOTIFY_IMAGE_SIZE * sizeof(uint16_t));

    jpeg_http_stream_ctx_t ctx = {
        .client = client,
        .out_buf = out_buf,
        .target_w = SPOTIFY_IMAGE_SIZE,
        .target_h = SPOTIFY_IMAGE_SIZE,
        .offset_x = 0,
        .offset_y = 0,
    };

    JDEC jd;
    JRESULT res = jd_prepare(&jd, http_jpeg_feed, workbuf, workbuf_size, &ctx);
    if (res != JDR_OK) {
        ESP_LOGE(TAG, "jd_prepare failed: %d", res);
        free(workbuf);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    ESP_LOGI(TAG, "Source JPEG: %ux%u (content_length=%lld). Decoding directly into framebuffer...",
             jd.width, jd.height, (long long)content_length);

    uint8_t scale = 0;
    if (jd.width >= 1000) {
        scale = 3; // 1:8
    } else if (jd.width >= 500) {
        scale = 2; // 1:4
    } else if (jd.width >= 250) {
        scale = 1; // 1:2
    } else {
        scale = 0; // 1:1
    }

    int scaled_w = jd.width >> scale;
    int scaled_h = jd.height >> scale;
    ctx.offset_x = (SPOTIFY_IMAGE_SIZE - scaled_w) / 2;
    ctx.offset_y = (SPOTIFY_IMAGE_SIZE - scaled_h) / 2;

    res = jd_decomp(&jd, jpeg_stream_out_cb, scale);

    free(workbuf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (res != JDR_OK) {
        ESP_LOGE(TAG, "jd_decomp failed: %d", res);
        return false;
    }

    ESP_LOGI(TAG, "Album art streamed and decoded successfully!");
    return true;
}

static bool refresh_access_token(void)
{
    spotify_config_t cfg;
    if (config_manager_get(&cfg) != ESP_OK || strlen(cfg.refresh_token) == 0) {
        ESP_LOGW(TAG, "No refresh token available");
        return false;
    }

    ESP_LOGI(TAG, "Refreshing access token against accounts.spotify.com...");
    char *post_data = malloc(1280);
    char *resp_buf = malloc(2048);
    if (!post_data || !resp_buf) {
        free(post_data);
        free(resp_buf);
        return false;
    }
    memset(resp_buf, 0, 2048);

    snprintf(post_data, 1280,
             "grant_type=refresh_token&refresh_token=%s&client_id=%s&client_secret=%s",
             cfg.refresh_token, cfg.client_id, cfg.client_secret);

    http_response_buffer_t resp = {
        .buf = resp_buf,
        .len = 0,
        .capacity = 2047,
    };

    esp_http_client_config_t config = {
        .url = "https://accounts.spotify.com/api/token",
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(post_data);

    if (err != ESP_OK || status_code != 200) {
        ESP_LOGE(TAG, "Token refresh failed: err=%d status=%d", err, status_code);
        free(resp_buf);
        return false;
    }

    cJSON *root = cJSON_Parse(resp.buf);
    free(resp_buf);
    if (!root) return false;

    cJSON *tok_item = cJSON_GetObjectItem(root, "access_token");
    cJSON *exp_item = cJSON_GetObjectItem(root, "expires_in");

    if (tok_item && tok_item->valuestring) {
        strncpy(s_access_token, tok_item->valuestring, sizeof(s_access_token) - 1);
        int expires_in = exp_item ? exp_item->valueint : 3600;
        s_token_expires_at_us = esp_timer_get_time() + ((int64_t)(expires_in - 180) * 1000000LL);
        ESP_LOGI(TAG, "Access token refreshed successfully (expires in %ds)", expires_in);
        cJSON_Delete(root);
        return true;
    }

    cJSON_Delete(root);
    return false;
}

bool spotify_service_exchange_code(const char *code, const char *redirect_uri)
{
    spotify_config_t cfg;
    if (config_manager_get(&cfg) != ESP_OK) return false;

    ESP_LOGI(TAG, "Exchanging auth code with accounts.spotify.com...");
    char *post_data = malloc(1536);
    char *resp_buf = malloc(2048);
    if (!post_data || !resp_buf) {
        free(post_data);
        free(resp_buf);
        return false;
    }
    memset(resp_buf, 0, 2048);

    snprintf(post_data, 1536,
             "grant_type=authorization_code&code=%s&redirect_uri=%s&client_id=%s&client_secret=%s",
             code, redirect_uri, cfg.client_id, cfg.client_secret);

    http_response_buffer_t resp = {
        .buf = resp_buf,
        .len = 0,
        .capacity = 2047,
    };

    esp_http_client_config_t config = {
        .url = "https://accounts.spotify.com/api/token",
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(post_data);

    if (err != ESP_OK || status_code != 200) {
        ESP_LOGE(TAG, "Code exchange failed: err=%d status=%d resp=%s", err, status_code, resp.buf);
        free(resp_buf);
        return false;
    }

    cJSON *root = cJSON_Parse(resp.buf);
    free(resp_buf);
    if (!root) return false;

    cJSON *tok_item = cJSON_GetObjectItem(root, "access_token");
    cJSON *ref_item = cJSON_GetObjectItem(root, "refresh_token");
    cJSON *exp_item = cJSON_GetObjectItem(root, "expires_in");

    if (ref_item && ref_item->valuestring) {
        config_manager_set_refresh_token(ref_item->valuestring);
        ESP_LOGI(TAG, "Refresh token saved to NVS!");
    }

    if (tok_item && tok_item->valuestring) {
        strncpy(s_access_token, tok_item->valuestring, sizeof(s_access_token) - 1);
        int expires_in = exp_item ? exp_item->valueint : 3600;
        s_token_expires_at_us = esp_timer_get_time() + ((int64_t)(expires_in - 180) * 1000000LL);
        cJSON_Delete(root);
        return true;
    }

    cJSON_Delete(root);
    return false;
}

static void poll_currently_playing(void)
{
    if (strlen(s_access_token) == 0) return;

    char *resp_buf = malloc(4096);
    if (!resp_buf) return;
    memset(resp_buf, 0, 4096);

    http_response_buffer_t resp = {
        .buf = resp_buf,
        .len = 0,
        .capacity = 4095,
    };

    esp_http_client_config_t config = {
        .url = "https://api.spotify.com/v1/me/player/currently-playing?additional_types=episode&market=from_token",
        .method = HTTP_METHOD_GET,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 5000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    char auth_hdr[600];
    snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", s_access_token);
    esp_http_client_set_header(client, "Authorization", auth_hdr);

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Currently playing request failed: %d", err);
        free(resp_buf);
        return;
    }

    if (status_code == 204) {
        s_current_state.has_track = false;
        s_current_state.is_playing = false;
        s_current_state.image_loaded = false;
        s_current_state.image_updated = false;
        s_current_state.image_url[0] = '\0';
        s_last_track_id[0] = '\0';
        s_current_state.track_name[0] = '\0';
        free(resp_buf);
        if (s_state_cb) s_state_cb(&s_current_state);
        return;
    }

    if (status_code != 200 || resp.len == 0) {
        free(resp_buf);
        return;
    }

    if (resp.len >= resp.capacity - 1) {
        ESP_LOGW(TAG, "Response buffer reached capacity (%d bytes), JSON may be truncated!", (int)resp.len);
    }

    s_last_poll_time_us = esp_timer_get_time();

    cJSON *root = cJSON_Parse(resp.buf);
    free(resp_buf);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse currently-playing JSON!");
        return;
    }

    cJSON *playing_item = cJSON_GetObjectItem(root, "is_playing");
    cJSON *progress_item = cJSON_GetObjectItem(root, "progress_ms");
    cJSON *item = cJSON_GetObjectItem(root, "item");

    if (item && cJSON_IsObject(item)) {
        s_current_state.has_track = true;
        s_current_state.is_playing = playing_item ? cJSON_IsTrue(playing_item) : false;
        s_current_state.progress_ms = progress_item ? progress_item->valueint : 0;

        cJSON *dur_item = cJSON_GetObjectItem(item, "duration_ms");
        s_current_state.duration_ms = dur_item ? dur_item->valueint : 0;

        char pending_img_url[256] = {0};
        bool need_img_download = false;
        bool track_changed = false;

        cJSON *id_item = cJSON_GetObjectItem(item, "id");
        const char *new_id = (id_item && id_item->valuestring) ? id_item->valuestring : "";

        cJSON *name_item = cJSON_GetObjectItem(item, "name");
        char new_track[128] = {0};
        if (name_item && name_item->valuestring) {
            snprintf(new_track, sizeof(new_track), "%s", name_item->valuestring);
        }

        if (!s_current_state.has_track ||
            (new_id[0] != '\0' && strcmp(s_last_track_id, new_id) != 0) ||
            strcmp(s_current_state.track_name, new_track) != 0) {
            track_changed = true;
            snprintf(s_current_state.track_name, sizeof(s_current_state.track_name), "%s", new_track);
            if (new_id[0] != '\0') {
                snprintf(s_last_track_id, sizeof(s_last_track_id), "%s", new_id);
            }
            ESP_LOGI(TAG, "Track changed to: '%s' (id=%s), dur=%lu ms (%lu:%02lu)",
                     new_track, new_id,
                     (unsigned long)s_current_state.duration_ms,
                     (unsigned long)(s_current_state.duration_ms / 60000),
                     (unsigned long)((s_current_state.duration_ms / 1000) % 60));
        }

        // Artists
        cJSON *artists = cJSON_GetObjectItem(item, "artists");
        if (artists && cJSON_GetArraySize(artists) > 0) {
            cJSON *art0 = cJSON_GetArrayItem(artists, 0);
            cJSON *art_name = cJSON_GetObjectItem(art0, "name");
            if (art_name && art_name->valuestring) {
                snprintf(s_current_state.artist_name, sizeof(s_current_state.artist_name), "%s", art_name->valuestring);
            }
        }

        // Album & Image
        cJSON *album = cJSON_GetObjectItem(item, "album");
        if (album) {
            cJSON *alb_name = cJSON_GetObjectItem(album, "name");
            if (alb_name && alb_name->valuestring) {
                snprintf(s_current_state.album_name, sizeof(s_current_state.album_name), "%s", alb_name->valuestring);
            }

            cJSON *images = cJSON_GetObjectItem(album, "images");
            if (images && cJSON_GetArraySize(images) > 0) {
                // Find image closest to 300px
                cJSON *best_img = cJSON_GetArrayItem(images, 0);
                int best_diff = 9999;
                for (int i = 0; i < cJSON_GetArraySize(images); i++) {
                    cJSON *candidate = cJSON_GetArrayItem(images, i);
                    cJSON *w = cJSON_GetObjectItem(candidate, "width");
                    int width_val = w ? w->valueint : 300;
                    int diff = abs(width_val - 300);
                    if (diff < best_diff) {
                        best_diff = diff;
                        best_img = candidate;
                    }
                }
                cJSON *url_item = cJSON_GetObjectItem(best_img, "url");
                if (url_item && url_item->valuestring) {
                    if (track_changed || strcmp(s_current_state.image_url, url_item->valuestring) != 0 || !s_current_state.image_loaded) {
                        snprintf(pending_img_url, sizeof(pending_img_url), "%s", url_item->valuestring);
                        need_img_download = true;
                    } else {
                        s_current_state.image_updated = false;
                    }
                }
            }
        }

        // Free cJSON tree immediately to reclaim heap before downloading image
        cJSON_Delete(root);
        root = NULL;

        // If track changed, immediately publish metadata to UI so title/artist update with ZERO delay!
        if (track_changed) {
            s_current_state.image_loaded = false;
            s_current_state.image_updated = false;
            if (s_state_cb) {
                s_state_cb(&s_current_state);
            }
        }

        if (need_img_download) {
            uint16_t *album_buf = ui_view_get_image_buffer();
            if (download_album_art(pending_img_url, album_buf)) {
                snprintf(s_current_state.image_url, sizeof(s_current_state.image_url), "%s", pending_img_url);
                s_current_state.image_loaded = true;
                s_current_state.image_updated = true;
            } else {
                ESP_LOGW(TAG, "Album art download will retry on next poll");
                s_current_state.image_loaded = false;
                s_current_state.image_updated = false;
            }
            if (s_state_cb) {
                s_state_cb(&s_current_state);
            }
        } else if (!track_changed) {
            if (s_state_cb) {
                s_state_cb(&s_current_state);
            }
        }
    }

    if (root) {
        cJSON_Delete(root);
    }
}

static void send_player_command(spotify_user_cmd_t cmd)
{
    if (strlen(s_access_token) == 0) return;

    const char *url = NULL;
    esp_http_client_method_t method = HTTP_METHOD_POST;

    switch (cmd) {
        case SPOTIFY_CMD_PLAY_PAUSE:
            url = s_current_state.is_playing ? "https://api.spotify.com/v1/me/player/pause"
                                             : "https://api.spotify.com/v1/me/player/play";
            method = HTTP_METHOD_PUT;
            s_current_state.is_playing = !s_current_state.is_playing;
            if (s_state_cb) s_state_cb(&s_current_state);
            break;
        case SPOTIFY_CMD_NEXT:
            url = "https://api.spotify.com/v1/me/player/next";
            method = HTTP_METHOD_POST;
            break;
        case SPOTIFY_CMD_PREV:
            url = "https://api.spotify.com/v1/me/player/previous";
            method = HTTP_METHOD_POST;
            break;
        default:
            return;
    }

    esp_http_client_config_t config = {
        .url = url,
        .method = method,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 4000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    char auth_hdr[600];
    snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", s_access_token);
    esp_http_client_set_header(client, "Authorization", auth_hdr);
    esp_http_client_set_header(client, "Content-Length", "0");

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "Sent player control %d: err=%d status=%d", cmd, err, status_code);
}

static void spotify_worker_task(void *arg)
{
    uint32_t poll_delay_ms = 1500;

    while (1) {
        // 1. Process pending user control commands
        spotify_user_cmd_t cmd;
        bool did_skip = false;
        while (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
            send_player_command(cmd);
            if (cmd == SPOTIFY_CMD_NEXT || cmd == SPOTIFY_CMD_PREV) {
                did_skip = true;
            }
        }

        if (did_skip) {
            // Give Spotify Connect ~350ms to advance playback state on player device
            vTaskDelay(pdMS_TO_TICKS(350));
        }

        // 2. Refresh token if expired or close to expiry
        int64_t now_us = esp_timer_get_time();
        if (strlen(s_access_token) == 0 || now_us >= s_token_expires_at_us) {
            refresh_access_token();
        }

        // 3. Poll current song state
        poll_currently_playing();

        // 4. Calculate adaptive poll delay:
        // - Spotify crossfade can be configured up to 12s, plus Automix and fade-in/fade-out transitions.
        // - Estimate real-time progress between polls using elapsed local time.
        // - In transition zone (remaining <= 14s OR progress <= 8s): fast poll (600ms)
        // - During normal playback: 1000ms
        // - When paused / idle: 2500ms
        poll_delay_ms = 2500;
        if (s_current_state.is_playing && s_current_state.duration_ms > 0) {
            int64_t elapsed_ms = (esp_timer_get_time() - s_last_poll_time_us) / 1000;
            int32_t est_progress_ms = (int32_t)s_current_state.progress_ms + (int32_t)elapsed_ms;
            int32_t est_remaining_ms = (int32_t)s_current_state.duration_ms - est_progress_ms;

            // Spotify Automix and Crossfade dynamically trim up to 25-35s from intros/outros.
            // Entering fast poll at 35s remaining guarantees we catch early Automix cuts instantly.
            if (est_remaining_ms <= 35000 || est_progress_ms <= 8000) {
                poll_delay_ms = 600; // In crossfade/fade/Automix transition window! Poll rapidly.
            } else {
                poll_delay_ms = 1000; // Normal playback polling
            }
        }

        // 5. Wait poll_delay_ms or wake up immediately if a user touch command arrives
        if (xQueueReceive(s_cmd_queue, &cmd, pdMS_TO_TICKS(poll_delay_ms)) == pdTRUE) {
            send_player_command(cmd);
            if (cmd == SPOTIFY_CMD_NEXT || cmd == SPOTIFY_CMD_PREV) {
                vTaskDelay(pdMS_TO_TICKS(350));
            }
        }
    }
}

void spotify_service_send_cmd(spotify_user_cmd_t cmd)
{
    if (s_cmd_queue) {
        xQueueSend(s_cmd_queue, &cmd, 0);
    }
}

esp_err_t spotify_service_init(spotify_state_callback_t cb)
{
    s_state_cb = cb;
    s_cmd_queue = xQueueCreate(8, sizeof(spotify_user_cmd_t));

    // Core 0 background worker task
    xTaskCreatePinnedToCore(spotify_worker_task, "spotify_worker", 8192, NULL, 4, NULL, 0);
    ESP_LOGI(TAG, "Spotify service started on Core 0");
    return ESP_OK;
}

