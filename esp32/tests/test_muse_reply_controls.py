# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Plain reply text, replay geometry/controls, and stopped-exchange retention."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / 'components/muse'


class ReplyControls(unittest.TestCase):
    def compile(self, out, code, text=False):
        source = out / 'test.c'
        source.write_text(code)
        command = [*shlex.split(os.environ.get('CC', 'cc')), '-std=gnu11',
                   '-fsanitize=address,undefined', '-include', str(ROOT / 'tests/host_compat.h'),
                   '-I' + str(MUSE), str(source)]
        if text:
            command.append(str(MUSE / 'muse_text.c'))
        command += ['-lm', '-o', str(out / 'test')]
        proc = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        return out / 'test'

    def test_plain_text_removes_formatting_without_losing_content(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = self.compile(Path(tmp), r'''
#include <stdio.h>
#include <string.h>
#include "muse_text.h"
int main(void) {
    char text[32769]; size_t n=fread(text,1,sizeof(text)-1,stdin); text[n]=0;
    muse_text_plain_reply(text); fwrite(text,1,strlen(text),stdout); return 0;
}
''', text=True)
            cases = [
                ('## A **bold** thought\n- *Hello*, __world__!\n> Nice ~~old~~ idea.',
                 'A bold thought\nHello, world!\nNice old idea.'),
                ('Read [this **guide**](https://example.test/a(b)) now.', 'Read this guide now.'),
                ('![A sunflower](https://example.test/image.png)', 'A sunflower'),
                ('```python\nprint("hi")\n```\nDone.', 'print("hi")\nDone.'),
                ('Use `snake_case` and 2 * 3 = 6; pay $5.50 & keep C#.',
                 'Use snake_case and 2 * 3 = 6; pay $5.50 & keep C#.'),
                ('こんにちは **世界**! 🌻', 'こんにちは 世界! '),
                ('***\nTitle\n---\n| A | B |\n|---|---|\n', 'Title\n  A   B  \n'),
                ('**an unfinished bold chunk', 'an unfinished bold chunk'),
                ('', ''),
                ('word **bold** ' * 1500, 'word bold ' * 1500),
            ]
            for text, expected in cases:
                with self.subTest(text=text[:60]):
                    proc = subprocess.run([str(binary)], input=text, text=True, capture_output=True)
                    self.assertEqual(proc.returncode, 0, proc.stderr)
                    self.assertEqual(proc.stdout, expected)

    def test_round_window_and_pause_hold_replay_controls(self):
        source = (MUSE / 'muse_ui.c').read_text()
        gestures = source[source.index('static void clear_cancel_hold('):source.index('static const lv_font_t *font_pick')]
        replay = source[source.index('static void on_replay_close('):source.index('static void build_replay(')]
        tick = source[source.index('static void update_replay('):source.index('static volatile bool s_snapshot;')]
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include "muse_text.h"
#define CANCEL_HOLD_US 5000000LL
#define MUSE_LOW_MEM 0
#define MUSE_REPLAY_TEXT_CAP 17408
#define MUSE_BIG_CAPS 0
#define LV_OBJ_FLAG_HIDDEN 1
#define LV_ANIM_OFF 0
#define MUSE_MODE_IDLE 0
#define MUSE_MODE_SPEAKING 1
#define MUSE_MODE_THINKING 2
#define MUSE_MODE_LISTENING 3
#define LV_EVENT_PRESSED 1
#define LV_EVENT_RELEASED 2
#define LV_EVENT_PRESS_LOST 3
#define LV_EVENT_SHORT_CLICKED 4
typedef int muse_mode_t;
typedef struct { bool hidden; char text[512]; } lv_obj_t;
typedef struct { int code; void *target; } lv_event_t;
static lv_obj_t canvas, button, panel, body, label, status;
static lv_obj_t *s_canvas=&canvas, *s_replay_btn=&button, *s_replay_panel=&panel;
static lv_obj_t *s_replay_body=&body, *s_replay_text=&label, *s_replay_status=&status;
static bool s_cancel_holding, s_replay_open, paused, asleep, available=true, active, face=true;
static int64_t s_cancel_hold_started, now;
static int mode=MUSE_MODE_SPEAKING, canceled, replayed;
static char caption[128];
static bool s_small;
static int s_w, s_h;
static struct { bool round; } board;
static const typeof(board) *muse_board=&board;
static bool muse_voice_paused(void) { return paused; }
static bool muse_state_asleep(void) { return asleep; }
static bool muse_ui_face_active(void) { return face; }
static int muse_state_mode(void *p) { return mode; }
static int64_t esp_timer_get_time(void) { return now; }
static void muse_state_set_caption(const char *fmt, ...) { va_list a; va_start(a,fmt); vsnprintf(caption,sizeof(caption),fmt,a); va_end(a); }
static void muse_voice_cancel_response(void) { canceled++; paused=false; active=false; }
static void muse_voice_toggle_pause(void) { paused=!paused; }
static void muse_state_make_happy(void) {}
static int lv_event_get_code(lv_event_t *e) { return e->code; }
static void *lv_event_get_target(lv_event_t *e) { return e->target; }
static void *heap_caps_malloc(size_t size, int caps) { return malloc(size); }
static size_t muse_voice_copy_last_answer(char *text, size_t cap) { return strlcpy(text,"You: Question\nReply: **A good answer**",cap); }
static void lv_obj_add_flag(lv_obj_t *o, int f) { o->hidden=true; }
static void lv_obj_remove_flag(lv_obj_t *o, int f) { o->hidden=false; }
static void lv_obj_set_flag(lv_obj_t *o, int f, bool v) { o->hidden=v; }
static void lv_label_set_text(lv_obj_t *o, const char *s) { strlcpy(o->text,s,sizeof(o->text)); }
static const char *lv_label_get_text(lv_obj_t *o) { return o->text; }
static void lv_obj_update_layout(lv_obj_t *o) {}
static void lv_obj_scroll_to_y(lv_obj_t *o, int y, int a) {}
static void lv_obj_move_foreground(lv_obj_t *o) {}
static void muse_voice_replay_last(void) { replayed++; active=true; }
static bool muse_voice_replaying(void) { return active; }
static bool muse_voice_has_last_answer(void) { return available; }
''' + gestures + replay + tick + r'''
static void event(int code) { lv_event_t e={code,s_canvas}; on_canvas_event(&e); }
int main(void) {
    int sizes[][2]={{466,466},{412,412},{240,240},{128,128},{135,240},{800,480}};
    for(int i=0;i<6;i++) {
        s_w=sizes[i][0]; s_h=sizes[i][1]; s_small=s_w<300 || s_h<200; board.round=i<3;
        int w,h; replay_panel_size(&w,&h); assert(w>0 && h>0 && w<s_w && h<s_h);
        if(board.round) {
            double radius=(s_w<s_h?s_w:s_h)/2.0-(s_small?8:16);
            assert((double)w*w/4+(double)h*h/4 <= radius*radius); /* all four corners fit */
        }
    }
    now=1000000;
    event(LV_EVENT_PRESSED); assert(!s_cancel_holding); event(LV_EVENT_RELEASED); event(LV_EVENT_SHORT_CLICKED);
    assert(paused);
    event(LV_EVENT_PRESSED); assert(s_cancel_holding && strstr(caption,"5s"));
    now+=4000000; update_cancel_hold(); assert(strstr(caption,"1s") && !canceled);
    event(LV_EVENT_RELEASED); assert(paused && !s_cancel_holding);
    now+=10000000; update_cancel_hold(); assert(!canceled);
    event(LV_EVENT_PRESSED); now+=5000000; update_cancel_hold(); assert(canceled==1 && !s_cancel_holding);
    event(LV_EVENT_RELEASED); update_cancel_hold(); assert(canceled==1);
    paused=true; event(LV_EVENT_PRESSED); event(LV_EVENT_PRESS_LOST); now+=5000000; update_cancel_hold(); assert(canceled==1);
    paused=false; mode=MUSE_MODE_IDLE;
    update_replay(mode); assert(!s_replay_btn->hidden);
    on_replay_click(NULL); assert(s_replay_open && s_replay_btn->hidden && !s_replay_panel->hidden && replayed==1);
    assert(strstr(s_replay_text->text,"You: Question") && !strchr(s_replay_text->text,'*'));
    update_replay(mode); assert(s_replay_open && s_replay_btn->hidden); /* ticking cannot unhide the replay button */
    on_replay_click(NULL); assert(s_replay_open && replayed==1); /* no toggle-close path */
    on_replay_close(NULL); assert(!s_replay_open && s_replay_panel->hidden);
    update_replay(mode); assert(!s_replay_btn->hidden);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            binary = self.compile(Path(tmp), code, text=True)
            proc = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_stopped_exchange_keeps_prompt_and_available_reply(self):
        source = (MUSE / 'muse_voice.c').read_text()
        save = source[source.index('static void save_last_answer('):source.index('void muse_voice_cancel_response(')]
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#define MUSE_LOW_MEM 0
#define MUSE_REPLAY_TEXT_CAP 17408
#define MUSE_BIG_CAPS 0
#define portMAX_DELAY 0
static void *s_last_lock;
static char *s_last_text;
static int16_t *s_last_pcm;
static size_t s_last_frames;
static atomic_bool s_last_available;
static const char *prompt="Explain sunflowers", *reply="Sunflowers follow sunlight.";
static void *heap_caps_malloc(size_t n, int caps) { return malloc(n); }
static void xSemaphoreTake(void *p, int wait) {}
static void xSemaphoreGive(void *p) {}
static size_t muse_hatch_turn_prompt_text(char *out, size_t cap) { strlcpy(out,prompt,cap); return strlen(out); }
static size_t muse_hatch_turn_reply_text(char *out, size_t cap) { strlcpy(out,reply,cap); return strlen(out); }
''' + save + r'''
int main(void) {
    int16_t *audio=calloc(10,sizeof(int16_t)); save_last_answer(audio,10);
    assert(atomic_load(&s_last_available) && s_last_pcm==audio && s_last_frames==10);
    assert(strstr(s_last_text,"You:\nExplain sunflowers") && strstr(s_last_text,"Reply:\nSunflowers follow sunlight."));
    reply=""; prompt="My new question"; save_last_answer(NULL,0);
    assert(strstr(s_last_text,"My new question") && !strstr(s_last_text,"Sunflowers follow sunlight"));
    assert(strstr(s_last_text,"stopped before a reply"));
    free(s_last_text); free(s_last_pcm); return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            binary = self.compile(Path(tmp), code)
            proc = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(proc.returncode, 0, proc.stderr)
