# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Compile the production provider codecs and settings against host fakes."""
import contextlib
import io
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
JSON = Path(os.environ.get('CJSON_SOURCE_DIR', ROOT / 'managed_components/espressif__cjson/cJSON'))
sys.path.insert(0, str(ROOT / 'tools/muse'))
import model_key


class ModelFirmware(unittest.TestCase):
    def test_codecs_and_persistent_settings(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            (out / 'freertos').mkdir()
            (out / 'esp_err.h').write_text('''#pragma once
#include <stdlib.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_SIZE 2
#define ESP_ERR_NVS_NO_FREE_PAGES 3
#define ESP_ERR_NVS_NEW_VERSION_FOUND 4
#define ESP_ERROR_CHECK(x) do { if (x) abort(); } while(0)
''')
            (out / 'esp_log.h').write_text('#define ESP_LOGI(...) ((void)0)\n#define ESP_LOGW(...) ((void)0)\n')
            (out / 'freertos/FreeRTOS.h').write_text('#pragma once\n#define portMAX_DELAY 0\n')
            (out / 'freertos/semphr.h').write_text('''#pragma once
typedef int SemaphoreHandle_t;
static inline int xSemaphoreCreateMutex(void) { return 1; }
static inline void xSemaphoreTake(int a, int b) { (void)a; (void)b; }
static inline void xSemaphoreGive(int a) { (void)a; }
''')
            (out / 'nvs.h').write_text('''#pragma once
#include <stddef.h>
#include <stdint.h>
typedef int nvs_handle_t;
#define NVS_READWRITE 0
int nvs_open(const char *, int, int *);
int nvs_get_str(int, const char *, char *, size_t *);
int nvs_get_u8(int, const char *, uint8_t *);
int nvs_get_u16(int, const char *, uint16_t *);
int nvs_set_str(int, const char *, const char *);
int nvs_set_u8(int, const char *, uint8_t);
int nvs_set_u16(int, const char *, uint16_t);
int nvs_commit(int);
''')
            (out / 'nvs_flash.h').write_text('int nvs_flash_init(void);\nint nvs_flash_erase(void);\n')
            harness = out / 'test.c'
            harness.write_text(r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "muse_model.h"
static char saved_key[512], saved_model[96];
static uint8_t saved_provider, saved_pet_sound;
static bool have_pet_sound;
static int fail_write, fail_commit, notifications;
int nvs_flash_init(void) { return 0; }
int nvs_flash_erase(void) { return 0; }
int nvs_open(const char *s, int m, int *h) { *h=1; return 0; }
int nvs_get_str(int h, const char *key, char *v, size_t *n) {
    const char *s = !strcmp(key,"model_key1") ? saved_key : !strcmp(key,"model_id1") ? saved_model : NULL;
    if (!s || !s[0]) return 1;
    snprintf(v,*n,"%s",s); return 0;
}
int nvs_get_u8(int h, const char *key, uint8_t *v) { if(!strcmp(key,"pet_sound") && have_pet_sound) { *v=saved_pet_sound; return 0; } if(strcmp(key,"provider")) return 1; *v=saved_provider; return 0; }
int nvs_get_u16(int h, const char *key, uint16_t *v) { return 1; }
int nvs_set_str(int h, const char *key, const char *v) {
    if(fail_write) return 9;
    if(!strcmp(key,"model_key1")) strcpy(saved_key,v);
    if(!strcmp(key,"model_id1")) strcpy(saved_model,v);
    return 0;
}
int nvs_set_u8(int h, const char *key, uint8_t v) { if(fail_write) return 9; if(!strcmp(key,"provider")) saved_provider=v; if(!strcmp(key,"pet_sound")) { have_pet_sound=true; saved_pet_sound=v; } return 0; }
int nvs_set_u16(int h, const char *key, uint16_t v) { return 0; }
int nvs_commit(int h) { return fail_commit ? 9 : 0; }
void muse_clock_start(void) {}
bool muse_link_wifi_get(char *s, char *p) { return false; }
bool muse_link_wifi_set(const char *s, const char *p) { return false; }
static void changed(muse_setting_t what) { assert(what==MUSE_SETTING_HATCH); notifications++; }
static void history_codec(void) {
    muse_model_history_t h={0};
    assert(muse_model_history_append(&h,"My pet is named Mochi.","Mochi is a great name!"));
    for(int p=1;p<MUSE_PROVIDER_COUNT;p++) {
        char *body=muse_model_body_with_history(p,"test-model","What is my pet's name?",h.messages);
        assert(body);
        cJSON *root=cJSON_Parse(body); free(body);
        cJSON *messages=cJSON_GetObjectItem(root,p==MUSE_PROVIDER_GEMINI ? "contents":"messages");
        assert(cJSON_GetArraySize(messages)==3);
        const char *roles[]={"user",p==MUSE_PROVIDER_GEMINI ? "model":"assistant","user"};
        const char *texts[]={"My pet is named Mochi.","Mochi is a great name!","What is my pet's name?"};
        for(int i=0;i<3;i++) {
            cJSON *m=cJSON_GetArrayItem(messages,i);
            assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(m,"role")),roles[i]));
            cJSON *t=p==MUSE_PROVIDER_GEMINI ? cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(m,"parts"),0),"text") : cJSON_GetObjectItem(m,"content");
            assert(!strcmp(cJSON_GetStringValue(t),texts[i]));
        }
        cJSON_Delete(root);
    }
    for(int i=0;i<10;i++) {
        char prompt[16]; snprintf(prompt,sizeof(prompt),"turn-%d",i);
        assert(muse_model_history_append(&h,prompt,"answer"));
    }
    assert(h.pairs==8 && cJSON_GetArraySize(h.messages)==16);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(h.messages,0),"content")),"turn-2"));
    muse_model_history_clear(&h);
    char large[10001]; memset(large,'a',10000); large[10000]=0;
    for(int i=0;i<4;i++) assert(muse_model_history_append(&h,large,large));
    assert(h.pairs==1 && h.bytes==20000); /* byte budget evicts whole exchanges */
    char *too_large=malloc(MUSE_MODEL_HISTORY_BYTES+1);
    memset(too_large,'b',MUSE_MODEL_HISTORY_BYTES); too_large[MUSE_MODEL_HISTORY_BYTES]=0;
    assert(!muse_model_history_append(&h,too_large,"answer"));
    assert(!h.messages && !h.pairs && !h.bytes); free(too_large);
    assert(!muse_model_history_append(&h,"question",NULL));
    cJSON *bad=cJSON_Parse("[{\"role\":\"assistant\",\"content\":\"wrong\"}]");
    assert(!muse_model_body_with_history(MUSE_PROVIDER_GEMINI,"test","next",bad)); cJSON_Delete(bad);
    muse_model_history_clear(&h);
}
static void codec(void) {
    for(int p=1;p<MUSE_PROVIDER_COUNT;p++) {
        char *body=muse_model_body(p,"test-model","hi\n\"世界"); assert(body);
        cJSON *root=cJSON_Parse(body); assert(root); free(body);
        cJSON *arr=cJSON_GetObjectItem(root,p==MUSE_PROVIDER_GEMINI ? "contents":"messages");
        cJSON *message=cJSON_GetArrayItem(arr,0);
        cJSON *value=cJSON_GetObjectItem(message,"content");
        if(p==MUSE_PROVIDER_GEMINI) value=cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(message,"parts"),0),"text");
        assert(!strcmp(cJSON_GetStringValue(value),"hi\n\"世界"));
        cJSON_Delete(root);
        assert(!muse_model_reply(p,NULL));
    }
    const char *responses[] = {
        "{\"candidates\":[{\"content\":{\"parts\":[{\"thought\":true,\"text\":\"private\"},{\"text\":\"Hello \"},{\"text\":\"world\"}]}}]}",
        "{\"choices\":[{\"message\":{\"content\":\"Hello world\"}}]}",
        "{\"content\":[{\"type\":\"thinking\",\"text\":\"private\"},{\"type\":\"text\",\"text\":\"Hello \"},{\"type\":\"tool_use\"},{\"type\":\"text\",\"text\":\"world\"}]}"
    };
    for(int p=1;p<4;p++) {
        cJSON *root=cJSON_Parse(responses[p-1]); char *reply=muse_model_reply(p,root);
        assert(reply && !strcmp(reply,"Hello world")); free(reply); cJSON_Delete(root);
        root=cJSON_Parse("{\"error\":{\"message\":\"secret\"}}");
        assert(!muse_model_reply(p,root)); cJSON_Delete(root);
    }
    assert(!muse_model_body(MUSE_PROVIDER_MUSE,"x","x"));
}
int main(void) {
    codec();
    history_codec();
    assert(!muse_settings_init());
    assert(muse_settings_pet_sound()); /* enabled by default */
    muse_settings_set_pet_sound(false);
    assert(!muse_settings_pet_sound() && have_pet_sound && !saved_pet_sound);
    saved_pet_sound=1; assert(!muse_settings_init()); assert(muse_settings_pet_sound());
    saved_pet_sound=0; assert(!muse_settings_init()); assert(!muse_settings_pet_sound());
    muse_settings_set_pet_sound(true);

    muse_settings_set_listener(changed);
    assert(muse_settings_provider()==MUSE_PROVIDER_MUSE);
    assert(!muse_settings_model_ready(MUSE_PROVIDER_GEMINI));
    assert(!muse_settings_set_model_key(MUSE_PROVIDER_GEMINI,"test-key"));
    assert(!muse_settings_set_model_id(MUSE_PROVIDER_GEMINI,"gemini-test"));
    assert(!muse_settings_set_provider(MUSE_PROVIDER_GEMINI));
    assert(notifications==3);
    char model[96], key[512];
    muse_settings_model(MUSE_PROVIDER_GEMINI,model,key);
    assert(!strcmp(model,"gemini-test") && !strcmp(key,"test-key"));
    assert(muse_settings_model_ready(MUSE_PROVIDER_GEMINI));
    assert(!muse_settings_init()); /* simulate reload of persisted values */
    assert(muse_settings_provider()==MUSE_PROVIDER_GEMINI);
    assert(muse_settings_set_model_key(MUSE_PROVIDER_GEMINI,"bad\r\nheader"));
    char too_long[513]; memset(too_long,'x',512); too_long[512]=0;
    assert(muse_settings_set_model_key(MUSE_PROVIDER_GEMINI,too_long));
    assert(muse_settings_set_model_id(MUSE_PROVIDER_GEMINI,"bad/path?key=secret"));
    assert(muse_settings_set_provider(99));
    fail_write=1;
    assert(muse_settings_set_model_key(MUSE_PROVIDER_GEMINI,"replacement"));
    assert(muse_settings_set_provider(MUSE_PROVIDER_OPENAI));
    fail_write=0; fail_commit=1;
    assert(muse_settings_set_model_key(MUSE_PROVIDER_GEMINI,"replacement"));
    assert(muse_settings_set_provider(MUSE_PROVIDER_OPENAI));
    fail_commit=0;
    muse_settings_model(MUSE_PROVIDER_GEMINI,model,key);
    assert(!strcmp(key,"test-key") && muse_settings_provider()==MUSE_PROVIDER_GEMINI);
    assert(notifications==3);
    assert(!muse_settings_set_model_key(MUSE_PROVIDER_GEMINI,""));
    assert(!muse_settings_model_ready(MUSE_PROVIDER_GEMINI));
    return 0;
}
''')
            cmd = [*shlex.split(os.environ.get('CC', 'cc')), '-std=gnu11',
                   '-DCONFIG_MUSE_DEFAULT_VOLUME=50', '-DCONFIG_MUSE_HATCH=1', '-include', str(ROOT / 'tests/host_compat.h'),
                   '-I' + str(out), '-I' + str(JSON), '-I' + str(ROOT / 'components/muse'),
                   str(harness), str(ROOT / 'components/muse/muse_model_codec.c'),
                   str(ROOT / 'components/muse/muse_settings.c'), str(JSON / 'cJSON.c'),
                   '-o', str(out / 'test')]
            subprocess.run(cmd, check=True, capture_output=True, text=True)
            subprocess.run([str(out / 'test')], check=True)

    def test_history_lifecycle_and_provider_isolation(self):
        source = (ROOT / 'components/muse/muse_model.c').read_text()
        state = source[source.index('#define RESPONSE_MAX'):source.index('static esp_err_t on_http(')]
        start = source[source.index('bool muse_model_init('):source.index('bool muse_model_poll(')]
        code = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "muse_model.h"
#define CONFIG_MUSE_BOARD_WAVESHARE_S3_175C 1
#include "muse_voice_limits.h"
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define portMAX_DELAY 0
#define pdPASS 1
#define MUSE_HATCH_WAV_HEADER 44
#define ESP_LOGW(...) ((void)0)
typedef int QueueHandle_t;
typedef int SemaphoreHandle_t;
static int xQueueCreate(int n, size_t size) { return 1; }
static int xSemaphoreCreateMutex(void) { return 1; }
static void xSemaphoreTake(int a, int b) {}
static void xSemaphoreGive(int a) {}
static void *heap_caps_malloc(size_t n, int caps) { return malloc(n); }
static void *heap_caps_calloc(size_t n, size_t size, int caps) { return calloc(n,size); }
static void muse_hatch_wav_header(uint8_t *out, uint32_t rate) { memset(out,0,44); }
''' + state + r'''
static job_t *active;
static bool task_ok=true;
static char selected_model[96]="model-a", selected_key[512]="key-a";
void muse_settings_model(muse_provider_t p, char model[MUSE_MODEL_ID_MAX+1], char key[MUSE_MODEL_KEY_MAX+1]) {
    strcpy(model,selected_model); strcpy(key,selected_key);
}
void muse_settings_azure_tts(char region[MUSE_AZURE_REGION_MAX+1], char key[MUSE_AZURE_KEY_MAX+1]) {
    strcpy(region,"eastus"); strcpy(key,"speech-key");
}
static void worker(void *arg) {}
static int xTaskCreatePinnedToCoreWithCaps(void (*fn)(void *), const char *name, int stack, void *arg, int priority, void *handle, int core, int caps) {
    if (!task_ok) return 0;
    active=arg; return pdPASS;
}
''' + start + r'''
static void begin(muse_provider_t provider, int pairs) {
    assert(muse_model_start(1,provider,"next",NULL,0));
    assert(cJSON_GetArraySize(active->history)==pairs*2);
}
static void complete(bool accepted) {
    active->result.prompt=active->prompt;
    active->result.text="reply";
    if(accepted) muse_model_accept(&active->result);
    free(active->prompt); free(active->wav); cJSON_Delete(active->history); free(active); active=NULL; s_busy=false;
}
int main(void) {
    assert(muse_model_init());
    begin(MUSE_PROVIDER_GEMINI,0); complete(false); /* canceled */
    begin(MUSE_PROVIDER_GEMINI,0); complete(true);
    begin(MUSE_PROVIDER_GEMINI,1); complete(true);
    begin(MUSE_PROVIDER_GEMINI,2);
    muse_model_forget(); complete(true); /* old in-flight reply cannot refill cleared history */
    begin(MUSE_PROVIDER_GEMINI,0); complete(true);
    begin(MUSE_PROVIDER_OPENAI,0); complete(true); /* never send Gemini context to OpenAI */
    begin(MUSE_PROVIDER_ANTHROPIC,0); complete(true);
    strcpy(selected_model,"model-b"); begin(MUSE_PROVIDER_ANTHROPIC,0); complete(true);
    strcpy(selected_key,"key-b"); begin(MUSE_PROVIDER_ANTHROPIC,0); complete(true);
    begin(MUSE_PROVIDER_ANTHROPIC,1); complete(false);
    muse_model_forget(); assert(!s_history.messages);
    /* Long recordings used to pass the session's cap, then fail this worker's
     * old 20-second guard and queue the same recording for endless retries. */
    uint8_t *pcm = malloc(16000 * 2 * 31);
    assert(pcm);
    for (size_t i=0; i<16000*2*31; i++) pcm[i]=(uint8_t)i;
    for (int seconds=21; seconds<=30; seconds+=9) {
        size_t bytes=16000*2*seconds;
        assert(muse_model_start(2,MUSE_PROVIDER_GEMINI,NULL,pcm,bytes));
        assert(active->provider==MUSE_PROVIDER_GEMINI);
        assert(active->wav_len==bytes+44);
        assert(!memcmp(active->wav+44,pcm,bytes));
        uint32_t riff, data;
        memcpy(&riff,active->wav+4,4); memcpy(&data,active->wav+40,4);
        assert(riff==bytes+36 && data==bytes);
        assert(!muse_model_start(3,MUSE_PROVIDER_GEMINI,NULL,pcm,bytes)); /* busy */
        complete(false);
    }
    assert(!muse_model_start(4,MUSE_PROVIDER_GEMINI,NULL,pcm,16000*2*31));
    assert(!muse_model_start(4,MUSE_PROVIDER_GEMINI,NULL,pcm,16000*2*30-1));
    /* Direct capture transfers its existing WAV allocation instead of making
     * another whole-recording copy at the busiest point in the turn. */
    size_t bytes=16000*2*30;
    uint8_t *wav=malloc(bytes+44);
    assert(wav); memcpy(wav+44,pcm,bytes);
    task_ok=false;
    assert(!muse_model_start_voice(5,MUSE_PROVIDER_GEMINI,wav,bytes));
    assert(!memcmp(wav+44,pcm,bytes)); /* failure retains caller ownership */
    task_ok=true;
    assert(muse_model_start_voice(5,MUSE_PROVIDER_GEMINI,wav,bytes));
    assert(active->wav==wav && active->wav_len==bytes+44);
    assert(!memcmp(active->wav+44,pcm,bytes));
    complete(false); /* success frees it once, on the worker side */
    free(pcm);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            (out / 'esp_err.h').write_text('typedef int esp_err_t;\n')
            (out / 'test.c').write_text(code)
            cmd = [*shlex.split(os.environ.get('CC', 'cc')), '-std=gnu11',
                   '-fsanitize=address,undefined', '-include', str(ROOT / 'tests/host_compat.h'),
                   '-I' + str(out), '-I' + str(JSON), '-I' + str(ROOT / 'components/muse'),
                   str(out / 'test.c'), str(ROOT / 'components/muse/muse_model_codec.c'), str(JSON / 'cJSON.c'),
                   '-o', str(out / 'test')]
            proc = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            subprocess.run([str(out / 'test')], check=True)

    def test_tls_chain_config_guard(self):
        source = (ROOT / 'cmake/validate_config.cmake').read_text()
        start = source.index('if(CONFIG_MUSE_HATCH AND NOT CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY)')
        end = source.index('endif()', start) + len('endif()')
        guard = source[start:end]
        with tempfile.TemporaryDirectory() as tmp:
            script = Path(tmp) / 'config.cmake'
            for ui, cross_signed, valid in [(True, True, True), (True, False, False), (False, False, True)]:
                with self.subTest(ui=ui, cross_signed=cross_signed):
                    script.write_text(f'set(CONFIG_MUSE_HATCH {int(ui)})\n'
                                      f'set(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY {int(cross_signed)})\n' + guard)
                    result = subprocess.run(['cmake', '-P', str(script)], capture_output=True, text=True)
                    self.assertEqual(result.returncode == 0, valid, result.stderr)
                    if not valid:
                        self.assertIn('CROSS_SIGNED_VERIFY=y', result.stderr)

    def test_canceled_replies_and_voice_capture(self):
        source = (ROOT / 'components/muse/muse_chat_session.cpp').read_text()
        pump = source[source.index('static void pump_model(void)'):source.index('#endif\n\n/* ---- Task ---- */')]
        code = r'''
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <atomic>
#define NOTE_MAX_BYTES 960000
#define MUSE_HATCH_WAV_HEADER 44
#define MIC_RATE 16000
#define MUSE_HATCH_EV_SENT 1
#define MUSE_HATCH_EV_HEARD 2
#define MUSE_HATCH_REACHABLE 3
#define MUSE_HATCH_UNREACHABLE 4
static bool s_direct=true, start_ok=true;
static int s_provider=1, s_in;
static uint32_t s_model_request=10;
static std::atomic<uint32_t> s_gen{7};
static uint8_t *s_model_pcm;
static size_t s_model_bytes, input_bytes;
enum phase_t { P_IDLE, P_LISTEN, P_WAIT_REPLY };
static struct { phase_t phase; uint32_t gen; bool text, end_requested;
    int64_t chat_us, last_event_us, last_content_us;
    char committed[512];
    struct msg_t { size_t len; } msgs[1];
} s_turn;
using msg_t=decltype(s_turn.msgs[0]);
struct muse_model_result_t { uint32_t request; char *text, *heard; char error[64]; char *prompt; };
static muse_model_result_t pending;
static bool has_result;
static int replies, failures, console_texts, started, accepted;
static void muse_model_accept(const muse_model_result_t *) { accepted++; }
static void muse_text_plain_reply(char *) {}
static size_t xStreamBufferReceive(int, void *, size_t room, int) {
    size_t n=input_bytes<room?input_bytes:room; input_bytes-=n; return n;
}
static bool muse_model_start_voice(uint32_t id, int, uint8_t *wav, size_t bytes) {
    assert(bytes>=9600); started++; if (start_ok) free(wav); return start_ok;
}
static bool muse_model_poll(muse_model_result_t *out) {
    if(!has_result) return false; *out=pending; has_result=false; return true;
}
static void turn_fail(const char *) { failures++; s_turn.phase=P_IDLE; }
static void emit(int, const char *) {}
static int64_t now_us() { return 123; }
static void muse_hatch_report(int, const char *) {}
static const char *muse_provider_name(int) { return "Gemini"; }
static int bind_msg(const char *, void *) { return 0; }
static void muse_hatch_console(const char *, const char *, const char *, int) { console_texts++; }
static void append_text(msg_t &, const char *text) { assert(!strcmp(text,"answer")); replies++; }
static void show_reply_start(msg_t &) {}
static void message_done(int, const char *) {}
''' + pump + r'''
static void result(uint32_t request, const char *error="") {
    pending={request,strdup("answer"),strdup("question"),{},strdup("question")};
    strcpy(pending.error,error); has_result=true; pump_model();
}
int main() {
    s_turn.phase=P_WAIT_REPLY; s_turn.gen=7;
    result(9); assert(!replies && !failures && !accepted); // replaced request
    s_gen=8; result(10); assert(!replies && !failures && !accepted); // cancelled voice generation
    s_gen=7; result(10); assert(replies==1 && accepted==1);
    s_turn.text=true; result(10); assert(console_texts==1 && replies==1);
    result(10,"CHECK API KEY"); assert(failures==1 && s_turn.phase==P_IDLE && accepted==2);
    s_turn={}; s_turn.phase=P_LISTEN; s_turn.end_requested=true;
    s_model_pcm=(uint8_t *)malloc(NOTE_MAX_BYTES + MUSE_HATCH_WAV_HEADER); input_bytes=16000;
    pump_model(); assert(started==1 && s_turn.phase==P_WAIT_REPLY && !s_model_pcm && s_model_request==11);
    s_turn.phase=P_LISTEN; s_model_bytes=0; input_bytes=100;
    s_model_pcm=(uint8_t *)malloc(NOTE_MAX_BYTES + MUSE_HATCH_WAV_HEADER);
    pump_model(); assert(failures==2 && started==1); free(s_model_pcm);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            (out / 'test.cpp').write_text(code)
            subprocess.run([*shlex.split(os.environ.get('CXX', 'c++')), '-std=c++17',
                            str(out / 'test.cpp'), '-o', str(out / 'test')], check=True, capture_output=True)
            subprocess.run([str(out / 'test')], check=True)


class ModelUpload(unittest.TestCase):
    def run_upload(self, args, key='test-key', ack='@model ok'):
        sent = []
        class Board:
            def __init__(self, port): pass
            def __enter__(self): return self
            def __exit__(self, *args): pass
            def status(self): return {'board':'test', 'chat':True}
            def drain(self, secs): pass
            def write_line(self, line): sent.append(line)
            def read_line(self, deadline): return ack
        stdout, stderr = io.StringIO(), io.StringIO()
        with patch.object(model_key, 'Board', Board), patch.object(model_key, 'pick_port', return_value='test'), \
             patch.object(sys, 'argv', ['model_key.py', *args]), \
             patch.object(model_key.getpass, 'getpass', return_value=key), \
             contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            result = model_key.main()
        self.assertNotIn(key, stdout.getvalue() + stderr.getvalue())
        return result, sent

    def test_hidden_upload_and_selection_order(self):
        result, sent = self.run_upload(['gemini','--model','gemini-test','--select'])
        self.assertEqual(result, 0)
        self.assertEqual(sent, ['model.gemini.key=test-key','model.gemini.id=gemini-test','model.provider=gemini'])

    def test_clear_and_restore_muse(self):
        self.assertEqual(self.run_upload(['openai','--clear'])[1], ['model.openai.key='])
        self.assertEqual(self.run_upload(['muse'])[1], ['model.provider=muse'])

    def test_rejected_write_does_not_select(self):
        result, sent = self.run_upload(['anthropic','--select'], ack='@model error')
        self.assertEqual(result, 1)
        self.assertEqual(sent, ['model.anthropic.key=test-key'])

    def test_invalid_key_never_transmitted(self):
        result, sent = self.run_upload(['openai'], key='bad\nkey')
        self.assertEqual(result, 1)
        self.assertEqual(sent, [])
