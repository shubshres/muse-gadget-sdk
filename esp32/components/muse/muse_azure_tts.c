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

#include "muse_azure_tts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "muse_settings.h"
#include "muse_text.h"

static const char *TAG = "muse_azure_tts";
#define MAX_MP3 (512 * 1024)
#define MAX_TEXT MUSE_AZURE_TTS_TEXT_MAX

typedef struct {
    muse_azure_tts_result_t result;
    char region[MUSE_AZURE_REGION_MAX + 1];
    char key[MUSE_AZURE_KEY_MAX + 1];
    char text[MAX_TEXT + 1];
    bool overflow;
} job_t;

static QueueHandle_t s_results;
static SemaphoreHandle_t s_lock;
static bool s_busy;

static bool valid_region(const char *s)
{
    if (!s[0]) return false;
    for (; *s; s++) {
        if (!(*s >= 'a' && *s <= 'z') && !(*s >= '0' && *s <= '9')) return false;
    }
    return true;
}

static char *ssml(const char *text)
{
    static const char prefix[] = "<speak version='1.0' xml:lang='en-US'><voice name='en-US-AnaNeural'>";
    static const char suffix[] = "</voice></speak>";
    size_t n = strlen(text), cap = sizeof(prefix) + sizeof(suffix) + n * 6;
    char *out = malloc(cap);
    if (!out) return NULL;
    char *p = out;
    memcpy(p, prefix, sizeof(prefix) - 1); p += sizeof(prefix) - 1;
    for (size_t i = 0; i < n; i++) {
        const char *esc = NULL;
        switch (text[i]) {
        case '&': esc = "&amp;"; break;
        case '<': esc = "&lt;"; break;
        case '>': esc = "&gt;"; break;
        case '\'': esc = "&apos;"; break;
        case '"': esc = "&quot;"; break;
        }
        if (esc) { size_t m = strlen(esc); memcpy(p, esc, m); p += m; }
        else *p++ = text[i];
    }
    memcpy(p, suffix, sizeof(suffix));
    return out;
}

static esp_err_t on_http(esp_http_client_event_t *event)
{
    job_t *job = event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || !event->data_len) return ESP_OK;
    if (job->result.length + event->data_len > MAX_MP3) {
        job->overflow = true;
        return ESP_FAIL;
    }
    memcpy(job->result.audio + job->result.length, event->data, event->data_len);
    job->result.length += event->data_len;
    return ESP_OK;
}

static void worker(void *arg)
{
    job_t *job = arg;
    bool success = false;
    char url[128];
    snprintf(url, sizeof(url), "https://%s.tts.speech.microsoft.com/cognitiveservices/v1", job->region);
    char *body = ssml(job->text);
    job->result.audio = heap_caps_malloc(MAX_MP3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (body && job->result.audio) {
        esp_http_client_config_t cfg = {
            .url = url,
            .timeout_ms = 20000,
            .buffer_size = 4096,
            .buffer_size_tx = 4096,
            .event_handler = on_http,
            .user_data = job,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .disable_auto_redirect = true,
        };
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (client) {
            esp_http_client_set_method(client, HTTP_METHOD_POST);
            esp_http_client_set_header(client, "Ocp-Apim-Subscription-Key", job->key);
            esp_http_client_set_header(client, "Content-Type", "application/ssml+xml");
            esp_http_client_set_header(client, "X-Microsoft-OutputFormat", "audio-16khz-32kbitrate-mono-mp3");
            esp_http_client_set_header(client, "User-Agent", "MuseGadget");
            esp_http_client_set_post_field(client, body, strlen(body));
            esp_err_t err = esp_http_client_perform(client);
            job->result.status = esp_http_client_get_status_code(client);
            success = err == ESP_OK && job->result.status == 200 && !job->overflow && job->result.length;
            if (!success) {
                ESP_LOGW(TAG, "synthesis failed (HTTP %d, %s)", job->result.status, esp_err_to_name(err));
            }
            esp_http_client_cleanup(client);
        }
    }
    if (!success) {
        free(job->result.audio);
        job->result.audio = NULL;
        job->result.length = 0;
    }
    free(body);
    memset(job->key, 0, sizeof(job->key));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_busy = false;
    xSemaphoreGive(s_lock);
    xQueueSend(s_results, &job->result, portMAX_DELAY);
    free(job);
    vTaskDelete(NULL);
}

bool muse_azure_tts_init(void)
{
    s_results = xQueueCreate(4, sizeof(muse_azure_tts_result_t));
    s_lock = xSemaphoreCreateMutex();
    return s_results && s_lock;
}

bool muse_azure_tts_start(uint32_t generation, int message, const char *text)
{
    if (!s_lock || !s_results || !text || !text[0] || strlen(text) > MAX_TEXT) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_busy) { xSemaphoreGive(s_lock); return false; }
    s_busy = true;
    xSemaphoreGive(s_lock);

    job_t *job = heap_caps_calloc(1, sizeof(*job), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (job) {
        job->result.generation = generation;
        job->result.message = message;
        muse_settings_azure_tts(job->region, job->key);
        strlcpy(job->text, text, sizeof(job->text));
        muse_text_plain_reply(job->text);
        if (!valid_region(job->region) || !job->key[0] || !job->text[0] ||
            xTaskCreatePinnedToCoreWithCaps(worker, "azure_tts", 12 * 1024, job, 4, NULL, 0,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            memset(job->key, 0, sizeof(job->key));
            free(job);
            job = NULL;
        }
    }
    if (!job) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_busy = false;
        xSemaphoreGive(s_lock);
    }
    return job != NULL;
}

bool muse_azure_tts_poll(muse_azure_tts_result_t *result)
{
    return s_results && xQueueReceive(s_results, result, 0) == pdTRUE;
}

bool muse_azure_tts_busy(void)
{
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool busy = s_busy;
    xSemaphoreGive(s_lock);
    return busy;
}
