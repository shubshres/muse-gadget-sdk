/* Copyright (c) Meta Platforms, Inc. and affiliates.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "muse_model.h"
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
#include "muse_chat_priv.h"
#include "muse_voice_limits.h"

#define RESPONSE_MAX (64 * 1024)
#define PCM_MAX MUSE_VOICE_INPUT_MAX_BYTES

typedef struct {
    muse_model_result_t result;
    muse_provider_t provider;
    char model[MUSE_MODEL_ID_MAX + 1], key[MUSE_MODEL_KEY_MAX + 1];
    char region[MUSE_AZURE_REGION_MAX + 1], speech_key[MUSE_AZURE_KEY_MAX + 1];
    char *prompt;
    cJSON *history;
    uint8_t *wav;
    size_t wav_len;
    char *response;
    size_t response_len;
    bool overflow;
} job_t;
static QueueHandle_t s_results;
static SemaphoreHandle_t s_lock;
static bool s_busy;
static muse_model_history_t s_history;
static uint32_t s_conversation = 1;
static muse_provider_t s_context_provider;
static char s_context_model[MUSE_MODEL_ID_MAX + 1], s_context_key[MUSE_MODEL_KEY_MAX + 1];

static void forget_locked(void)
{
    muse_model_history_clear(&s_history);
    s_conversation++;
    s_context_provider = MUSE_PROVIDER_MUSE;
    memset(s_context_model, 0, sizeof(s_context_model));
    memset(s_context_key, 0, sizeof(s_context_key));
}

void muse_model_forget(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    forget_locked();
    xSemaphoreGive(s_lock);
}

void muse_model_accept(const muse_model_result_t *result)
{
    if (!s_lock || result->error[0] || !result->prompt || !result->text) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (result->conversation == s_conversation &&
        !muse_model_history_append(&s_history, result->prompt, result->text)) {
        ESP_LOGW("muse_model", "Exchange could not be retained in recent conversation");
    }
    xSemaphoreGive(s_lock);
}

static esp_err_t on_http(esp_http_client_event_t *event)
{
    job_t *job = event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) return ESP_OK;
    if ((size_t)event->data_len > RESPONSE_MAX - job->response_len) {
        job->overflow = true;
        return ESP_FAIL;
    }
    memcpy(job->response + job->response_len, event->data, event->data_len);
    job->response_len += event->data_len;
    job->response[job->response_len] = 0;
    return ESP_OK;
}

static cJSON *request(job_t *job, const char *url, const char *body, size_t len, bool speech)
{
    job->response_len = 0;
    job->overflow = false;
    esp_http_client_config_t cfg = {
        .url = url, .timeout_ms = 30000, .buffer_size = 4096, .buffer_size_tx = 4096,
        .event_handler = on_http, .user_data = job,
        .crt_bundle_attach = esp_crt_bundle_attach, .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        snprintf(job->result.error, sizeof(job->result.error), "MODEL: OUT OF MEMORY");
        return NULL;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", speech ?
        "audio/wav; codecs=audio/pcm; samplerate=16000" : "application/json");
    char auth[MUSE_MODEL_KEY_MAX + 8];
    if (speech) esp_http_client_set_header(client, "Ocp-Apim-Subscription-Key", job->speech_key);
    else if (job->provider == MUSE_PROVIDER_GEMINI)
        esp_http_client_set_header(client, "x-goog-api-key", job->key);
    else if (job->provider == MUSE_PROVIDER_ANTHROPIC) {
        esp_http_client_set_header(client, "x-api-key", job->key);
        esp_http_client_set_header(client, "anthropic-version", "2023-06-01");
    } else {
        snprintf(auth, sizeof(auth), "Bearer %s", job->key);
        esp_http_client_set_header(client, "Authorization", auth);
    }
    esp_http_client_set_post_field(client, body, len);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    int tls_code = 0, tls_flags = 0;
    esp_err_t tls_err = esp_http_client_get_and_clear_last_tls_error(client, &tls_code, &tls_flags);
    if (err != ESP_OK) {
        ESP_LOGW("muse_model", "%s request failed: %s, HTTP %d, errno %d, TLS %s / %d, flags 0x%x",
                 speech ? "Speech" : muse_provider_name(job->provider), esp_err_to_name(err), status,
                 esp_http_client_get_errno(client), esp_err_to_name(tls_err), tls_code, (unsigned)tls_flags);
    }
    esp_http_client_cleanup(client);
    memset(auth, 0, sizeof(auth));
    if (err != ESP_OK || status != 200 || job->overflow) {
        snprintf(job->result.error, sizeof(job->result.error), "%s: %s (HTTP %d)",
                 speech ? "SPEECH" : muse_provider_name(job->provider),
                 job->overflow ? "REPLY TOO LARGE" :
                 err != ESP_OK && (tls_code || tls_flags) ? "TLS CERTIFICATE / CONNECTION ERROR" :
                 err != ESP_OK ? "NETWORK ERROR" :
                 status == 401 || status == 403 ? "CHECK API KEY" :
                 status == 429 ? "RATE LIMIT / QUOTA" : "REQUEST FAILED", status);
        return NULL;
    }
    cJSON *root = cJSON_ParseWithLength(job->response, job->response_len);
    if (!root) snprintf(job->result.error, sizeof(job->result.error), "INVALID API RESPONSE");
    return root;
}

static void worker(void *arg)
{
    job_t *job = arg;
    job->response = heap_caps_malloc(RESPONSE_MAX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!job->response) snprintf(job->result.error, sizeof(job->result.error), "MODEL: OUT OF MEMORY");
    if (job->response && job->wav) {
        char url[192];
        snprintf(url, sizeof(url), "https://%s.stt.speech.microsoft.com/speech/recognition/conversation/cognitiveservices/v1?language=en-US", job->region);
        cJSON *root = request(job, url, (const char *)job->wav, job->wav_len, true);
        const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(root, "DisplayText"));
        if (text && text[0]) {
            job->prompt = strdup(text);
            job->result.heard = strdup(text);
        } else if (!job->result.error[0]) {
            snprintf(job->result.error, sizeof(job->result.error), "DIDN'T CATCH THAT");
        }
        cJSON_Delete(root);
        free(job->wav);
        job->wav = NULL;   /* the transcript replaces the recording before the model request */
    }
    if (job->response && job->prompt && !job->result.error[0]) {
        char url[256];
        if (job->provider == MUSE_PROVIDER_GEMINI)
            snprintf(url, sizeof(url), "https://generativelanguage.googleapis.com/v1beta/models/%s:generateContent", job->model);
        else snprintf(url, sizeof(url), "%s", job->provider == MUSE_PROVIDER_OPENAI ?
            "https://api.openai.com/v1/chat/completions" : "https://api.anthropic.com/v1/messages");
        char *body = muse_model_body_with_history(job->provider, job->model, job->prompt, job->history);
        cJSON *root = body ? request(job, url, body, strlen(body), false) : NULL;
        job->result.text = muse_model_reply(job->provider, root);
        if (!job->result.text && !job->result.error[0])
            snprintf(job->result.error, sizeof(job->result.error), "NO TEXT REPLY FROM MODEL");
        cJSON_Delete(root);
        cJSON_free(body);
    }
    if (!job->result.text && !job->result.error[0])
        snprintf(job->result.error, sizeof(job->result.error), "MODEL: OUT OF MEMORY");
    free(job->response);
    free(job->wav);
    if (job->result.text && !job->result.error[0]) job->result.prompt = job->prompt;
    else free(job->prompt);
    cJSON_Delete(job->history);
    memset(job->key, 0, sizeof(job->key));
    memset(job->speech_key, 0, sizeof(job->speech_key));
    xQueueSend(s_results, &job->result, portMAX_DELAY);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_busy = false;
    xSemaphoreGive(s_lock);
    free(job);
    vTaskDelete(NULL);
}

bool muse_model_init(void)
{
    s_results = xQueueCreate(2, sizeof(muse_model_result_t));
    s_lock = xSemaphoreCreateMutex();
    return s_results && s_lock;
}

static bool model_start(uint32_t id, muse_provider_t provider, const char *text,
                        const uint8_t *pcm, size_t bytes, uint8_t *owned_wav)
{
    if (!s_lock || !s_results || provider <= MUSE_PROVIDER_MUSE || provider >= MUSE_PROVIDER_COUNT ||
        (!text && (!pcm || bytes < 9600 || bytes > PCM_MAX || bytes % 2))) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_busy) { xSemaphoreGive(s_lock); return false; }
    s_busy = true;
    xSemaphoreGive(s_lock);
    job_t *job = heap_caps_calloc(1, sizeof(*job), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool ok = false;
    if (job) {
        job->result.request = id;
        job->provider = provider;
        muse_settings_model(provider, job->model, job->key);
        muse_settings_azure_tts(job->region, job->speech_key);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_context_provider != provider || strcmp(s_context_model, job->model) || strcmp(s_context_key, job->key)) {
            forget_locked();
            s_context_provider = provider;
            strlcpy(s_context_model, job->model, sizeof(s_context_model));
            strlcpy(s_context_key, job->key, sizeof(s_context_key));
        }
        job->result.conversation = s_conversation;
        job->history = s_history.messages ? cJSON_Duplicate(s_history.messages, true) : cJSON_CreateArray();
        xSemaphoreGive(s_lock);
        bool speech = !text && pcm;
        if (text) job->prompt = strdup(text);
        else {
            job->wav_len = bytes + MUSE_HATCH_WAV_HEADER;
            job->wav = owned_wav ? owned_wav :
                heap_caps_malloc(job->wav_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (job->wav) {
                muse_hatch_wav_header(job->wav, 16000);
                uint32_t riff = bytes + 36, data = bytes;
                memcpy(job->wav + 4, &riff, 4);
                memcpy(job->wav + 40, &data, 4);
                if (!owned_wav) memcpy(job->wav + 44, pcm, bytes);
            }
        }
        bool region_ok = job->region[0];
        for (const char *p = job->region; *p; p++)
            if (!(*p >= 'a' && *p <= 'z') && !(*p >= '0' && *p <= '9')) region_ok = false;
        ok = job->history && job->key[0] && job->model[0] && (text ? job->prompt != NULL :
             job->wav && (!speech || (region_ok && job->speech_key[0]))) &&
             xTaskCreatePinnedToCoreWithCaps(worker, "muse_model", 16 * 1024, job, 4, NULL, 0,
                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
        if (!ok) {
            free(job->prompt);
            if (job->wav != owned_wav) free(job->wav);
            cJSON_Delete(job->history);
            memset(job, 0, sizeof(*job)); free(job);
        }
    }
    if (!ok) {
        xSemaphoreTake(s_lock, portMAX_DELAY); s_busy = false; xSemaphoreGive(s_lock);
    }
    return ok;
}

bool muse_model_start(uint32_t id, muse_provider_t provider, const char *text, const uint8_t *pcm, size_t bytes)
{
    return model_start(id, provider, text, pcm, bytes, NULL);
}

bool muse_model_start_voice(uint32_t id, muse_provider_t provider, uint8_t *wav, size_t bytes)
{
    return wav && model_start(id, provider, NULL, wav + MUSE_HATCH_WAV_HEADER, bytes, wav);
}

bool muse_model_poll(muse_model_result_t *out)
{
    return s_results && xQueueReceive(s_results, out, 0) == pdTRUE;
}
