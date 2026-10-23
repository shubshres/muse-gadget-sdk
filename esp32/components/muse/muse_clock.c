/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "muse_clock.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "muse_settings.h"
#include "muse_wifi.h"

/* The caller's public IP supplies the location. No credentials or device ID
 * are sent. Keep a private clock: do not change the Link's system time/TZ. */
#define CLOCK_URL "https://worldtime.timezone.io/api/ip"
#define RESPONSE_MAX 2048
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_local_epoch, s_synced_us;
static bool s_valid, s_started;

typedef struct {
    char data[RESPONSE_MAX];
    size_t len;
} response_t;

static esp_err_t on_http(esp_http_client_event_t *event)
{
    response_t *r = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_DATA) {
        if (event->data_len < 0 || (size_t)event->data_len >= sizeof(r->data) - r->len)
            return ESP_ERR_INVALID_SIZE;
        memcpy(r->data + r->len, event->data, event->data_len);
        r->len += event->data_len;
        r->data[r->len] = '\0';
    }
    return ESP_OK;
}

static bool sync_clock(void)
{
    response_t r = {0};
    esp_http_client_config_t config = {
        .url = CLOCK_URL,
        .timeout_ms = 10000,
        .event_handler = on_http,
        .user_data = &r,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return false;
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200) return false;
    cJSON *root = cJSON_Parse(r.data);
    const cJSON *epoch = cJSON_GetObjectItemCaseSensitive(root, "unixtime");
    const cJSON *raw = cJSON_GetObjectItemCaseSensitive(root, "raw_offset");
    const cJSON *dst = cJSON_GetObjectItemCaseSensitive(root, "dst_offset");
    bool valid = cJSON_IsNumber(epoch) && epoch->valuedouble >= 1704067200 &&
                 epoch->valuedouble <= 4102444800 &&
                 cJSON_IsNumber(raw) && raw->valuedouble >= -43200 && raw->valuedouble <= 50400 &&
                 cJSON_IsNumber(dst) && dst->valuedouble >= -7200 && dst->valuedouble <= 7200;
    if (valid) {
        int64_t local = (int64_t)epoch->valuedouble + (int64_t)raw->valuedouble + (int64_t)dst->valuedouble;
        portENTER_CRITICAL(&s_lock);
        s_local_epoch = local;
        s_synced_us = esp_timer_get_time();
        s_valid = true;
        portEXIT_CRITICAL(&s_lock);
    }
    cJSON_Delete(root);
    return valid;
}

static void clock_task(void *arg)
{
    (void)arg;
    int64_t next = 0;
    bool was_connected = false, was_enabled = false;
    for (;;) {
        bool enabled = muse_settings_always_on();
        bool connected = muse_wifi_connected();
        int64_t now = esp_timer_get_time();
        if (enabled && connected && (!was_connected || !was_enabled || now >= next)) {
            bool ok = sync_clock();
            /* Hourly refresh follows travel and daylight saving changes.
             * Failed lookups back off a minute; never hold the UI or wake Wi-Fi. */
            next = esp_timer_get_time() + (ok ? 3600LL : 60LL) * 1000000;
            if (!ok) ESP_LOGW("muse_clock", "Local time unavailable; retrying in a minute");
        }
        was_connected = connected;
        was_enabled = enabled;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void muse_clock_start(void)
{
    if (!muse_settings_always_on()) return;
    portENTER_CRITICAL(&s_lock);
    bool start = !s_started;
    s_started = true;
    portEXIT_CRITICAL(&s_lock);
    if (start && xTaskCreate(clock_task, "muse_clock", 8192, NULL, 2, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_lock);
        s_started = false;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGE("muse_clock", "Could not start local clock lookup");
    }
}

bool muse_clock_format(char *clock, size_t clock_size, char *date, size_t date_size)
{
    portENTER_CRITICAL(&s_lock);
    bool valid = s_valid;
    time_t local = s_local_epoch + (esp_timer_get_time() - s_synced_us) / 1000000;
    portEXIT_CRITICAL(&s_lock);
    if (!valid) return false;
    struct tm tm;
    if (!gmtime_r(&local, &tm)) return false;
    int hour = tm.tm_hour % 12;
    snprintf(clock, clock_size, "%d:%02d", hour ? hour : 12, tm.tm_min);
    char weekday[8], month[8];
    strftime(weekday, sizeof(weekday), "%a", &tm);
    strftime(month, sizeof(month), "%b", &tm);
    snprintf(date, date_size, "%s %s %d", weekday, month, tm.tm_mday);
    return true;
}
