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

#include "muse_settings.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "muse_link.h"
#include "muse_clock.h"

static const char *TAG = "muse_settings";

#define NS "muse"
#define DEFAULT_HOST "hatch.metaaivm.com"

static struct {
    uint8_t volume;
    bool speaker_on;
    bool startup_sound;
    bool pet_sound;
    uint8_t mic_gain;
    uint8_t brightness;
    uint16_t sleep_s;
    bool always_on;
    bool motion_wake;
    bool motion_silence;
    bool wifi_on;
    bool ble_on;
    char ssid[MUSE_SSID_MAX + 1];
    char pass[MUSE_PASS_MAX + 1];
    char host[MUSE_HOST_MAX + 1];
    char vm[MUSE_VM_MAX + 1];
    char token[MUSE_TOKEN_MAX + 1];
#if CONFIG_MUSE_HATCH
    uint8_t provider;
    char model_keys[MUSE_PROVIDER_COUNT][MUSE_MODEL_KEY_MAX + 1];
    char model_ids[MUSE_PROVIDER_COUNT][MUSE_MODEL_ID_MAX + 1];
#endif
    char azure_region[MUSE_AZURE_REGION_MAX + 1];
    char azure_key[MUSE_AZURE_KEY_MAX + 1];
} s = {
    .volume = CONFIG_MUSE_DEFAULT_VOLUME,
    .speaker_on = true,
    .startup_sound = true,
    .pet_sound = true,
    .mic_gain = 30,
    .brightness = 100,
    .sleep_s = 120,
    .motion_wake = true,
    .motion_silence = true,
    .wifi_on = true,
    .host = DEFAULT_HOST,
    .azure_region = "eastus",
#if CONFIG_MUSE_HATCH
    .model_ids = { "", "gemini-3.5-flash-lite", "gpt-4.1-mini", "claude-sonnet-5-5" },
#endif
};

static SemaphoreHandle_t s_lock;
static nvs_handle_t s_nvs;
static muse_setting_cb_t s_listener;

#define LOCKED(body) do { xSemaphoreTake(s_lock, portMAX_DELAY); body; xSemaphoreGive(s_lock); } while (0)

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Missing keys leave the default in place. */
static void load_str(const char *key, char *out, size_t len)
{
    size_t n = len;
    nvs_get_str(s_nvs, key, out, &n);
}

static void load_u8(const char *key, uint8_t *out)
{
    nvs_get_u8(s_nvs, key, out);
}

static void save_u8(const char *key, uint8_t v)
{
    nvs_set_u8(s_nvs, key, v);
    nvs_commit(s_nvs);
}

static void save_str(const char *key, const char *v)
{
    nvs_set_str(s_nvs, key, v);
    nvs_commit(s_nvs);
}

static void notify(muse_setting_t what)
{
    if (s_listener) {
        s_listener(what);
    }
}

esp_err_t muse_settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS layout changed, erasing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }
    s_lock = xSemaphoreCreateMutex();
    err = nvs_open(NS, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t b;
    load_u8("volume", &s.volume);
    if (nvs_get_u8(s_nvs, "speaker", &b) == ESP_OK) {
        s.speaker_on = b;
    }
    if (nvs_get_u8(s_nvs, "startup_sound", &b) == ESP_OK) s.startup_sound = b;
    if (nvs_get_u8(s_nvs, "pet_sound", &b) == ESP_OK) s.pet_sound = b;
    load_u8("mic_gain", &s.mic_gain);
    load_u8("bright", &s.brightness);
    nvs_get_u16(s_nvs, "sleep_s", &s.sleep_s);
    if (nvs_get_u8(s_nvs, "always_on", &b) == ESP_OK) s.always_on = b;
    if (nvs_get_u8(s_nvs, "motion_wake", &b) == ESP_OK) s.motion_wake = b;
    if (nvs_get_u8(s_nvs, "motion_silence", &b) == ESP_OK) s.motion_silence = b;
    if (nvs_get_u8(s_nvs, "wifi_on", &b) == ESP_OK) {
        s.wifi_on = b;
    }
    if (nvs_get_u8(s_nvs, "ble_on", &b) == ESP_OK) {
        s.ble_on = b;
    }
    load_str("ssid", s.ssid, sizeof(s.ssid));
    load_str("pass", s.pass, sizeof(s.pass));
    load_str("host", s.host, sizeof(s.host));
    load_str("vm", s.vm, sizeof(s.vm));
    load_str("token", s.token, sizeof(s.token));
    load_str("az_region", s.azure_region, sizeof(s.azure_region));
    load_str("az_key", s.azure_key, sizeof(s.azure_key));

#if CONFIG_MUSE_HATCH
    load_u8("provider", &s.provider);
    if (s.provider >= MUSE_PROVIDER_COUNT) s.provider = MUSE_PROVIDER_MUSE;
    for (int i = 1; i < MUSE_PROVIDER_COUNT; i++) {
        char name[16];
        snprintf(name, sizeof(name), "model_key%d", i);
        load_str(name, s.model_keys[i], sizeof(s.model_keys[i]));
        snprintf(name, sizeof(name), "model_id%d", i);
        load_str(name, s.model_ids[i], sizeof(s.model_ids[i]));
    }
#endif
    s.volume = clampi(s.volume, 0, 100);
    s.mic_gain = clampi(s.mic_gain, 0, MUSE_MIC_GAIN_MAX);
    s.brightness = clampi(s.brightness, 10, 100);
    ESP_LOGI(TAG, "vol %d%s, mic %d dB, bright %d, sleep %ds, wifi %s (%s), ble %s, muse %s",
             s.volume, s.speaker_on ? "" : " (speaker off)", s.mic_gain, s.brightness, s.sleep_s, s.wifi_on ? "on" : "off",
             "network saved by Link", s.ble_on ? "on" : "off", s.token[0] ? "token set" : "no token");
    return ESP_OK;
}

void muse_settings_set_listener(muse_setting_cb_t cb)
{
    s_listener = cb;
}

int muse_settings_volume(void) { return s.volume; }
bool muse_settings_speaker_on(void) { return s.speaker_on; }
bool muse_settings_startup_sound(void) { return s.startup_sound; }
bool muse_settings_pet_sound(void) { return s.pet_sound; }
int muse_settings_mic_gain(void) { return s.mic_gain; }
int muse_settings_brightness(void) { return s.brightness; }
int muse_settings_sleep_s(void) { return s.sleep_s; }
bool muse_settings_always_on(void) { return s.always_on; }
bool muse_settings_motion_wake(void) { return s.motion_wake; }
bool muse_settings_motion_silence(void) { return s.motion_silence; }
bool muse_settings_wifi_on(void) { return s.wifi_on; }
bool muse_settings_ble_on(void) { return s.ble_on; }

/* Home Link owns the saved networks (this is the first); the local copy is only a fallback. */
void muse_settings_wifi(char ssid[MUSE_SSID_MAX + 1], char pass[MUSE_PASS_MAX + 1])
{
    if (muse_link_wifi_get(ssid, pass)) {
        return;
    }
    LOCKED({
        strlcpy(ssid, s.ssid, MUSE_SSID_MAX + 1);
        if (pass) {
            strlcpy(pass, s.pass, MUSE_PASS_MAX + 1);
        }
    });
}

void muse_settings_hatch_host(char out[MUSE_HOST_MAX + 1])
{
    LOCKED(strlcpy(out, s.host, MUSE_HOST_MAX + 1));
}

void muse_settings_hatch_vm(char out[MUSE_VM_MAX + 1])
{
    LOCKED(strlcpy(out, s.vm, MUSE_VM_MAX + 1));
}

void muse_settings_hatch_token(char out[MUSE_TOKEN_MAX + 1])
{
    LOCKED(strlcpy(out, s.token, MUSE_TOKEN_MAX + 1));
}

size_t muse_settings_hatch_token_len(void)
{
    size_t n;
    LOCKED(n = strlen(s.token));
    return n;
}

void muse_settings_azure_tts(char region[MUSE_AZURE_REGION_MAX + 1],
                             char key[MUSE_AZURE_KEY_MAX + 1])
{
    LOCKED({
        strlcpy(region, s.azure_region, MUSE_AZURE_REGION_MAX + 1);
        strlcpy(key, s.azure_key, MUSE_AZURE_KEY_MAX + 1);
    });
}

bool muse_settings_azure_tts_ready(void)
{
    bool ready;
    LOCKED(ready = s.azure_region[0] && s.azure_key[0]);
    return ready;
}

void muse_settings_set_volume(int pct)
{
    s.volume = clampi(pct, 0, 100);
    save_u8("volume", s.volume);
    notify(MUSE_SETTING_VOLUME);
}

void muse_settings_set_speaker_on(bool on)
{
    s.speaker_on = on;
    save_u8("speaker", on);
    notify(MUSE_SETTING_SPEAKER);
}

void muse_settings_set_startup_sound(bool on)
{
    s.startup_sound = on;
    save_u8("startup_sound", on);
}

void muse_settings_set_pet_sound(bool on)
{
    s.pet_sound = on;
    save_u8("pet_sound", on);
}

void muse_settings_set_mic_gain(int db)
{
    s.mic_gain = clampi(db, 0, MUSE_MIC_GAIN_MAX);
    save_u8("mic_gain", s.mic_gain);
    notify(MUSE_SETTING_MIC_GAIN);
}

void muse_settings_set_brightness(int pct)
{
    s.brightness = clampi(pct, 10, 100);
    save_u8("bright", s.brightness);
    notify(MUSE_SETTING_BRIGHTNESS);
}

void muse_settings_set_sleep_s(int secs)
{
    s.sleep_s = clampi(secs, 0, 3600);
    nvs_set_u16(s_nvs, "sleep_s", s.sleep_s);
    nvs_commit(s_nvs);
    notify(MUSE_SETTING_SLEEP);
}

void muse_settings_set_always_on(bool on)
{
    s.always_on = on;
    save_u8("always_on", on);
    if (on) muse_clock_start();
}

void muse_settings_set_motion_wake(bool on)
{
    s.motion_wake = on;
    save_u8("motion_wake", on);
    notify(MUSE_SETTING_MOTION_WAKE);
}

void muse_settings_set_motion_silence(bool on)
{
    s.motion_silence = on;
    save_u8("motion_silence", on);
    notify(MUSE_SETTING_MOTION_SILENCE);
}

void muse_settings_set_wifi_on(bool on)
{
    s.wifi_on = on;
    save_u8("wifi_on", on);
    notify(MUSE_SETTING_WIFI);
}

void muse_settings_set_ble_on(bool on)
{
    s.ble_on = on;
    save_u8("ble_on", on);
    notify(MUSE_SETTING_BLE);
}

void muse_settings_set_wifi(const char *ssid, const char *pass)
{
    if (muse_link_wifi_set(ssid ? ssid : "", ssid && ssid[0] && pass ? pass : "")) {
        ESP_LOGI(TAG, "wifi network: %s", ssid && ssid[0] ? ssid : "(all forgotten)");
        notify(MUSE_SETTING_WIFI);
        return;
    }
    LOCKED({
        strlcpy(s.ssid, ssid ? ssid : "", sizeof(s.ssid));
        strlcpy(s.pass, s.ssid[0] && pass ? pass : "", sizeof(s.pass));
        save_str("ssid", s.ssid);
        save_str("pass", s.pass);
    });
    ESP_LOGI(TAG, "wifi network: %s", s.ssid[0] ? s.ssid : "(forgotten)");
    notify(MUSE_SETTING_WIFI);
}

void muse_settings_set_hatch_host(const char *host)
{
    LOCKED({
        strlcpy(s.host, host && host[0] ? host : DEFAULT_HOST, sizeof(s.host));
        save_str("host", s.host);
    });
    notify(MUSE_SETTING_HATCH);
}

void muse_settings_set_hatch_vm(const char *vm)
{
    LOCKED({
        strlcpy(s.vm, vm ? vm : "", sizeof(s.vm));
        save_str("vm", s.vm);
    });
    notify(MUSE_SETTING_HATCH);
}

esp_err_t muse_settings_set_hatch_token(const char *token, bool append)
{
    esp_err_t err = ESP_OK;
    LOCKED({
        size_t have = append ? strlen(s.token) : 0;
        size_t add = strlen(token ? token : "");
        if (have + add > MUSE_TOKEN_MAX) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            memcpy(s.token + have, token, add);
            s.token[have + add] = '\0';
            save_str("token", s.token);
        }
    });
    if (err == ESP_OK) {
        notify(MUSE_SETTING_HATCH);
    }
    return err;
}

void muse_settings_set_azure_region(const char *region)
{
    LOCKED({
        strlcpy(s.azure_region, region ? region : "", sizeof(s.azure_region));
        save_str("az_region", s.azure_region);
    });
}

esp_err_t muse_settings_set_azure_key(const char *key)
{
    esp_err_t err;
    LOCKED({
        const char *value = key ? key : "";
        if (strlen(value) > MUSE_AZURE_KEY_MAX) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            err = nvs_set_str(s_nvs, "az_key", value);
            if (err == ESP_OK) err = nvs_commit(s_nvs);
            if (err == ESP_OK) strlcpy(s.azure_key, value, sizeof(s.azure_key));
        }
    });
    return err;
}

#if CONFIG_MUSE_HATCH
const char *muse_provider_name(muse_provider_t p)
{
    static const char *names[] = { "Muse", "Gemini", "OpenAI", "Anthropic" };
    return p >= 0 && p < MUSE_PROVIDER_COUNT ? names[p] : "Unknown";
}

const char *muse_provider_id(muse_provider_t p)
{
    static const char *ids[] = { "muse", "gemini", "openai", "anthropic" };
    return p >= 0 && p < MUSE_PROVIDER_COUNT ? ids[p] : "unknown";
}

muse_provider_t muse_settings_provider(void)
{
    muse_provider_t p;
    LOCKED(p = (muse_provider_t)s.provider);
    return p;
}

esp_err_t muse_settings_set_provider(muse_provider_t p)
{
    if (p < 0 || p >= MUSE_PROVIDER_COUNT) return ESP_ERR_INVALID_ARG;
    esp_err_t err;
    LOCKED({
        err = nvs_set_u8(s_nvs, "provider", p);
        if (err == ESP_OK) err = nvs_commit(s_nvs);
        if (err == ESP_OK) s.provider = p;
    });
    if (err == ESP_OK) notify(MUSE_SETTING_HATCH);
    return err;
}

void muse_settings_model(muse_provider_t p, char model[MUSE_MODEL_ID_MAX + 1],
                         char key[MUSE_MODEL_KEY_MAX + 1])
{
    if (model) model[0] = '\0';
    if (key) key[0] = '\0';
    if (p <= MUSE_PROVIDER_MUSE || p >= MUSE_PROVIDER_COUNT) return;
    LOCKED({
        if (model) strlcpy(model, s.model_ids[p], MUSE_MODEL_ID_MAX + 1);
        if (key) strlcpy(key, s.model_keys[p], MUSE_MODEL_KEY_MAX + 1);
    });
}

bool muse_settings_model_ready(muse_provider_t p)
{
    if (p <= MUSE_PROVIDER_MUSE || p >= MUSE_PROVIDER_COUNT) return false;
    bool ready;
    LOCKED(ready = s.model_ids[p][0] && s.model_keys[p][0]);
    return ready;
}

static esp_err_t set_model_value(muse_provider_t p, const char *value, bool key)
{
    if (p <= MUSE_PROVIDER_MUSE || p >= MUSE_PROVIDER_COUNT || !value) return ESP_ERR_INVALID_ARG;
    size_t n = strlen(value), max = key ? MUSE_MODEL_KEY_MAX : MUSE_MODEL_ID_MAX;
    if (n > max || (!key && !n)) return ESP_ERR_INVALID_SIZE;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = value[i];
        if (key ? (c < 33 || c > 126) :
                  !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
            return ESP_ERR_INVALID_ARG;
    }
    char name[16];
    snprintf(name, sizeof(name), key ? "model_key%d" : "model_id%d", p);
    esp_err_t err;
    LOCKED({
        err = nvs_set_str(s_nvs, name, value);
        if (err == ESP_OK) err = nvs_commit(s_nvs);
        if (err == ESP_OK) strlcpy(key ? s.model_keys[p] : s.model_ids[p], value, max + 1);
    });
    if (err == ESP_OK) notify(MUSE_SETTING_HATCH);
    return err;
}

esp_err_t muse_settings_set_model_key(muse_provider_t p, const char *key)
{
    return set_model_value(p, key, true);
}

esp_err_t muse_settings_set_model_id(muse_provider_t p, const char *model)
{
    return set_model_value(p, model, false);
}

#endif /* CONFIG_MUSE_HATCH */
