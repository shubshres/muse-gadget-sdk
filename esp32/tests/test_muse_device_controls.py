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
    start = re.search(r'(?:static )?(?:void|bool) ' + name + r'\([^)]*\)\n\{', source).start()
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
                '-Wall', '-Wextra', '-Werror', str(source), '-o', str(binary), '-lm',
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
        /* Irregular polls must still measure eight seconds of real time. */
        now += 1500000; aux_button(true, false); assert(shutdowns == 0);
        now += 6499999; aux_button(true, false); assert(shutdowns == 0);
        now++; aux_button(true, false); assert(shutdowns == 1);
        now += 1000000; aux_button(true, false); assert(shutdowns == 1);
        aux_button(false, true); assert(!asleep && shutdowns == 1);
    } else if (test == 2 || test == 3) {
        now += 7999999; aux_button(true, false);
        assert(strcmp(caption, "HOLD TO POWER OFF") == 0);
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

    def test_startup_chime_is_short_and_bounded(self):
        source = (MUSE / 'muse_audio.c').read_text()
        code = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#define MUSE_AUDIO_RATE 16000
#define MUSE_AUDIO_CHUNK 320
#define ESP_OK 0
static size_t frames, nonzero;
static int muse_audio_write(const int16_t *pcm, size_t n) {
    assert(n <= MUSE_AUDIO_CHUNK);
    for (size_t i = 0; i < n; i++) {
        assert(pcm[i] >= -5000 && pcm[i] <= 5000);
        nonzero += pcm[i] != 0;
    }
    frames += n;
    return ESP_OK;
}
''' + function(source, 'muse_audio_startup') + r'''
int main(void) {
    muse_audio_startup();
    assert(frames == MUSE_AUDIO_RATE * 3 / 10);
    assert(nonzero > frames / 2);
    return 0;
}
'''
        self.compile_run(code)

    def test_recording_runs_for_a_minute_or_until_release(self):
        source = (MUSE / 'muse_voice.c').read_text()
        constants = source[source.index('#if CONFIG_MUSE_BOARD_WAVESHARE_S3_175C\n#define MAX_SECS'):source.index('#define MIN_HELD_FRAMES')]
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
    assert(held == MUSE_AUDIO_RATE * 60 && s_rec_n == held);
    reads = 0; release_at = 1000; /* release after twenty seconds */
    assert(record(&held, why, sizeof(why)));
    assert(held >= MUSE_AUDIO_RATE * 20);
    assert(held <= MUSE_AUDIO_RATE * 20 + TAIL_FRAMES);
    return 0;
}
'''
        self.compile_run(code)
