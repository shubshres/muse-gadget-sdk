# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Exercise the production aux hold handler and startup chime on the host."""

import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest

MUSE = Path(__file__).resolve().parents[1] / 'components/muse'


def function(source, name):
    start = re.search(r'(?:static )?(?:void|bool|esp_err_t) ' + name + r'\([^)]*\)\n\{', source).start()
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class DeviceControls(unittest.TestCase):
    def compile_run(self, code, args=()):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / 'controls.c'
            binary = source.with_suffix('')
            source.write_text(code)
            compiled = subprocess.run([
                *shlex.split(os.environ.get('CC', 'cc')), '-std=gnu11',
                '-Wall', '-Wextra', '-Werror', '-I', str(MUSE), str(source), '-o', str(binary), '-lm',
            ], capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            for arg in args or ('',):
                ran = subprocess.run([str(binary), str(arg)], capture_output=True, text=True)
                self.assertEqual(ran.returncode, 0, ran.stderr)

    def test_bottom_hold_from_awake_and_asleep(self):
        source = (MUSE / 'muse_input.c').read_text()
        constants = source[source.index('#define HINT_HOLD_US'):source.index('#define SLEEP_CHECK_MS')]
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#define CONFIG_MUSE_BOARD_WAVESHARE_S3_175C 1
#define DOUBLE_TICKS 35
static int64_t now;
static bool asleep;
static int shutdowns, setups;
static char caption[64] = "READY";
static void power_off(void) { shutdowns++; }
static struct { const char *aux_button; void (*power_off)(void); } board = {"bottom", power_off};
static const typeof(board) *muse_board = &board;
static int64_t esp_timer_get_time(void) { return now; }
static bool muse_state_asleep(void) { return asleep; }
static void set_asleep(bool value, const char *why) { (void)why; asleep = value; }
static void toggle_phone_setup(void) { setups++; }
static void muse_state_caption(char *out, size_t cap, uint32_t *version) {
    (void)version; snprintf(out, cap, "%s", caption);
}
static void muse_state_set_caption(const char *fmt, ...) {
    va_list args; va_start(args, fmt); vsnprintf(caption, sizeof(caption), fmt, args); va_end(args);
}
''' + constants + function(source, 'aux_button') + r'''
int main(int argc, char **argv) {
    assert(argc == 2);
    int test = atoi(argv[1]);
    asleep = test == 1 || test == 3;
    now = 100000;
    aux_button(true, true);
    assert(!asleep && shutdowns == 0);
    if (test == 0 || test == 1) {
        /* Irregular polls must still measure five seconds of real time. */
        now += 600000; aux_button(true, false);
        assert(strcmp(caption, "POWER OFF IN 5") == 0);
        now += 400000; aux_button(true, false);
        assert(strcmp(caption, "POWER OFF IN 4") == 0);
        now += 500000; aux_button(true, false); assert(shutdowns == 0);
        now += 3499999; aux_button(true, false); assert(shutdowns == 0);
        now++; aux_button(true, false); assert(shutdowns == 1);
        now += 1000000; aux_button(true, false); assert(shutdowns == 1);
        aux_button(false, true); assert(!asleep && shutdowns == 1);
    } else if (test == 2 || test == 3) {
        now += 4999999; aux_button(true, false);
        assert(strcmp(caption, "POWER OFF IN 1") == 0);
        aux_button(false, true);
        assert(strcmp(caption, "READY") == 0 && shutdowns == 0);
    } else if (test == 4) {
        aux_button(false, true);
        for (int i = 0; i < 35; i++) aux_button(false, false);
        assert(asleep && shutdowns == 0);
    } else {
        aux_button(false, true);
        aux_button(true, true);
        assert(setups == 1);
        now += 9000000; aux_button(true, false);
        assert(shutdowns == 0 && !asleep);
    }
    return 0;
}
'''
        self.compile_run(code, range(6))

    def test_power_chimes_are_short_bounded_and_opposite(self):
        source = (MUSE / 'muse_audio.c').read_text()
        code = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <stdbool.h>
#define MUSE_AUDIO_RATE 16000
#define MUSE_AUDIO_CHUNK 320
#define ESP_OK 0
static size_t frames, nonzero;
static int crossings[3], previous;
static int muse_audio_write(const int16_t *pcm, size_t n) {
    assert(n <= MUSE_AUDIO_CHUNK);
    for (size_t i = 0; i < n; i++) {
        assert(pcm[i] >= -5000 && pcm[i] <= 5000);
        nonzero += pcm[i] != 0;
        if (pcm[i] && previous && ((pcm[i] < 0) != (previous < 0))) crossings[(frames + i) / 1600]++;
        if (pcm[i]) previous = pcm[i];
    }
    frames += n;
    return ESP_OK;
}
''' + '\n'.join(function(source, name) for name in ('power_chime', 'muse_audio_startup', 'muse_audio_shutdown')) + r'''
int main(void) {
    muse_audio_startup();
    assert(frames == MUSE_AUDIO_RATE * 3 / 10);
    assert(nonzero > frames / 2);
    assert(crossings[0] < crossings[1] && crossings[1] < crossings[2]);
    frames = nonzero = 0; previous = 0;
    for (int i = 0; i < 3; i++) crossings[i] = 0;
    muse_audio_shutdown();
    assert(frames == MUSE_AUDIO_RATE * 3 / 10 && nonzero > frames / 2);
    assert(crossings[0] > crossings[1] && crossings[1] > crossings[2]);
    return 0;
}
'''
        self.compile_run(code)

    def test_recording_runs_for_thirty_seconds_or_until_release(self):
        source = (MUSE / 'muse_voice.c').read_text()
        constants = source[source.index('#define MAX_SECS'):source.index('#define MIN_HELD_FRAMES')]
        stats = source[source.index('typedef struct {\n    double acc;'):source.index('/* Adds one captured chunk')]
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#define CONFIG_MUSE_BOARD_WAVESHARE_S3_175C 1
#include "muse_voice_limits.h"
#define MUSE_AUDIO_RATE 16000
#define MUSE_AUDIO_CHUNK 320
#define MUSE_MODE_LISTENING 1
#define MUSE_PTT_UP 1
#define MUSE_HATCH_EV_NONE 0
#define MUSE_HATCH_EV_HEARD 1
#define MUSE_HATCH_EV_ERROR 2
#define ESP_OK 0
typedef int muse_hatch_ev_t;
static int16_t *s_rec, s_chunk[MUSE_AUDIO_CHUNK];
static size_t s_rec_n, s_sent, s_pre_fill, reads, release_at;
static bool s_live, s_tried;
static bool s_power_off_requested;
#define atomic_load(p) (*(p))
static int s_held_count;
static const char *TAG = "voice";
static void log_stub(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
#define ESP_LOGI log_stub
#define ESP_LOGW log_stub
static bool muse_hatch_ready(void) { return true; }
static void go_live(void) { s_live = true; }
static void muse_state_set_mode(int mode) { (void)mode; }
static void muse_state_set_progress(float value) { assert(value >= 0 && value <= 1); }
static void muse_state_set_caption(const char *fmt, ...) { (void)fmt; }
static void muse_state_set_level(float level) { (void)level; }
static float muse_audio_level(const int16_t *pcm, size_t n) { (void)pcm; (void)n; return 0; }
static int muse_settings_mic_gain(void) { return 30; }
static int muse_audio_read(int16_t *pcm, size_t n) { memset(pcm, 0, n * sizeof(*pcm)); reads++; return ESP_OK; }
static int muse_hatch_turn_event(char *text, size_t cap) { (void)text; (void)cap; return MUSE_HATCH_EV_NONE; }
static void muse_hatch_turn_cancel(void) {}
static bool got_event(int type) { (void)type; return reads == release_at; }
static void pre_get(size_t i, int16_t *out) { (void)i; (void)out; assert(false); }
#undef strlcpy
#define strlcpy copy_string
static size_t copy_string(char *out, const char *in, size_t cap) { snprintf(out, cap, "%s", in); return strlen(in); }
''' + constants + stats + r'''
static void take(rec_stats_t *stats, const int16_t *pcm) {
    (void)stats; (void)pcm;
    s_rec_n += MUSE_AUDIO_CHUNK;
    assert(s_rec_n <= MAX_FRAMES);
}
''' + function(source, 'record') + r'''
int main(void) {
    int16_t dummy;
    s_rec = &dummy;
    char why[96]; size_t held;
    release_at = 0;
    assert(record(&held, why, sizeof(why)));
    assert(held == MUSE_AUDIO_RATE * 30 && s_rec_n == held);
    reads = 0; release_at = 1000; /* release after twenty seconds */
    assert(record(&held, why, sizeof(why)));
    assert(held >= MUSE_AUDIO_RATE * 20);
    assert(held <= MUSE_AUDIO_RATE * 20 + TAIL_FRAMES);
    s_power_off_requested = true;
    reads = 0;
    assert(record(&held, why, sizeof(why)));
    assert(reads == 0 && held == 0);
    return 0;
}
'''
        self.compile_run(code)

    def test_shutdown_audio_cancels_reply_and_closes_codecs(self):
        source = (MUSE / 'muse_voice.c').read_text()
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#define MUSE_MODE_OFF 9
static bool speaker, powered;
static int step, chimes;
static void muse_hatch_turn_cancel(void) { assert(step++ == 0); }
static void muse_audio_power(bool on) {
    if (on) assert(step++ == 1);
    else assert(step++ == (speaker ? 3 : 2));
    powered = on;
}
static bool muse_settings_speaker_on(void) { return speaker; }
static void muse_audio_shutdown(void) { assert(powered && step++ == 2); chimes++; }
static void muse_state_set_mode(int mode) { assert(!powered && mode == MUSE_MODE_OFF); }
static void muse_state_set_caption(const char *caption) { assert(strcmp(caption, "GOODBYE!") == 0); }
''' + function(source, 'power_off_audio') + r'''
int main(void) {
    speaker = true;
    power_off_audio();
    assert(chimes == 1 && !powered);
    step = 0; speaker = false;
    power_off_audio();
    assert(chimes == 1 && !powered);
    return 0;
}
'''
        self.compile_run(code)

    def test_talk_disables_both_pmu_cutoffs_and_checks_readback(self):
        source = (MUSE / 'muse_pmu.c').read_text()
        constants = source[source.index('#define AXP2101_ADDR'):source.index('static i2c_master_dev_handle_t')]
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE -2
typedef int esp_err_t;
#define ESP_RETURN_ON_ERROR(expr, ...) do { int err = (expr); if (err) return err; } while (0)
#define ESP_RETURN_ON_FALSE(expr, err, ...) do { if (!(expr)) return err; } while (0)
#define ESP_LOGI(...) ((void)0)
''' + constants + r'''
static void *s_dev = (void *)1;
static uint8_t regs[256];
static int fail_read = -1, fail_write = -1;
static bool stuck;
static int rd(uint8_t reg, uint8_t *value) {
    if (reg == fail_read) return -3;
    *value = regs[reg]; return ESP_OK;
}
static int wr(uint8_t reg, uint8_t value) {
    if (reg == REG_COMMON_CFG) assert(!(value & 0x03)); /* Never reset or soft-off here. */
    if (reg == fail_write) return -4;
    if (!stuck) regs[reg] = value;
    return ESP_OK;
}
''' + function(source, 'muse_pmu_set_key_shutdown') + r'''
int main(void) {
    for (int common = 0; common < 256; common++) {
        regs[REG_COMMON_CFG] = common;
        regs[REG_PWROFF_EN] = 0xff;
        assert(muse_pmu_set_key_shutdown(false) == ESP_OK);
        assert(regs[REG_COMMON_CFG] == (common & ~0x07));
        assert(regs[REG_PWROFF_EN] == 0xfd);
        assert(muse_pmu_set_key_shutdown(true) == ESP_OK);
        assert(regs[REG_PWROFF_EN] == 0xff);
    }
    regs[REG_COMMON_CFG] = 0x34; regs[REG_PWROFF_EN] = 0x01;
    stuck = true;
    assert(muse_pmu_set_key_shutdown(false) == ESP_FAIL); /* 16 s bit would otherwise survive. */
    regs[REG_COMMON_CFG] = 0x30; regs[REG_PWROFF_EN] = 0x03;
    assert(muse_pmu_set_key_shutdown(false) == ESP_FAIL); /* Normal cutoff readback too. */
    stuck = false; fail_write = REG_COMMON_CFG;
    assert(muse_pmu_set_key_shutdown(false) == -4);
    fail_write = -1; fail_read = REG_COMMON_CFG;
    assert(muse_pmu_set_key_shutdown(false) == -3);
    s_dev = NULL;
    assert(muse_pmu_set_key_shutdown(false) == ESP_ERR_INVALID_STATE);
    return 0;
}
'''
        self.compile_run(code)
