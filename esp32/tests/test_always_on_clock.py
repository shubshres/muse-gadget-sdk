# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Exercise the real sleep transition code with a fake display and clock."""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class AlwaysOnClockTest(unittest.TestCase):
    def test_sleep_delay_fades_and_taps_do_not_skip(self):
        source = (ROOT / "components/muse/muse_ui.c").read_text()
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define LV_OBJ_FLAG_HIDDEN 1
#define LV_OPA_COVER 255
#define LV_OPA_TRANSP 0
#define LV_ALIGN_CENTER 0
#define LV_EVENT_PRESSED 1
#define LV_EVENT_RELEASED 2
typedef int lv_event_t;
typedef int lv_timer_t;
static int lv_event_get_code(lv_event_t *e) { return *e; }
typedef struct { char text[64]; int hidden, opa, text_opa, x, y; } obj;
static obj cover, ct, cd;
static obj *s_cover=&cover, *s_clock_time=&ct, *s_clock_date=&cd;
static bool s_dark, s_clock_lit, asleep, enabled, panel;
static int repaints;
static int s_w=466, s_h=466;
static bool s_small;
static int64_t now, state_sleep_started;
static int brightness, s_brightness=-1, s_preview_brightness=-1;
static struct { void (*panel_sleep)(bool); bool brightness_fade; } board;
static const __typeof__(board) *muse_board=&board;
static bool muse_state_asleep(void) { return asleep; }
static void muse_state_set_asleep(bool v) {
    if (v && !asleep) state_sleep_started=now;
    asleep=v;
}
static int64_t muse_state_sleep_started_us(void) { return state_sleep_started; }
static bool muse_settings_always_on(void) { return enabled; }
static int muse_settings_brightness(void) { return 80; }
static int64_t esp_timer_get_time(void) { return now; }
static void muse_menu_close(void) {}
static void apply_brightness(int v) { brightness=s_brightness=v; }
static void panel_sleep(bool v) { panel=v; }
static void lv_obj_set_style_bg_opa(obj *o,int v,int x) { (void)x; o->opa=v; }
static int lv_obj_get_style_bg_opa(obj *o,int x) { (void)x; return o->opa; }
static void lv_obj_set_style_opa(obj *o,int v,int x) { (void)x; o->text_opa=v; }
static void lv_obj_set_flag(obj *o,int f,bool v) { (void)f; o->hidden=v; }
static void lv_obj_add_flag(obj *o,int f) { lv_obj_set_flag(o,f,true); }
static void lv_obj_remove_flag(obj *o,int f) { lv_obj_set_flag(o,f,false); }
static const char *lv_label_get_text(obj *o) { return o->text; }
static void lv_label_set_text(obj *o,const char *s) { snprintf(o->text,sizeof(o->text),"%s",s); }
static void lv_obj_align(obj *o,int a,int x,int y) { (void)a; o->x=x; o->y=y; }
static void lv_obj_move_foreground(obj *o) { assert(o==s_cover); }
static void lv_obj_invalidate(obj *o) { assert(o==s_cover && !panel && o->opa>=0 && o->opa<=255); }
static obj *lv_obj_get_display(obj *o) { return o; }
static void lv_refr_now(obj *o) { assert(o==s_cover && !panel); repaints++; }
static bool muse_clock_format(char *t,size_t tn,char *d,size_t dn) {
    snprintf(t,tn,"2:23"); snprintf(d,dn,"Fri Oct 23"); return true;
}
'''
        globals_start = source.index("static int64_t s_sleep_started;")
        globals_end = source.index("#define SLEEP_FADE_US", globals_start)
        code += source[globals_start:source.index("\n", globals_end) + 1]
        for sig in ("static void on_cover_event(", "bool muse_ui_dark(void)",
                    "static void position_clock(", "static void set_clock_opacity(", "static void sleep_panel(",
                    "static void finish_sleep_fade(", "static int sleep_fade_progress(",
                    "static bool update_sleep(void)"):
            code += function(source, sig)
        code += r'''
int main(void) {
    lv_event_t press=LV_EVENT_PRESSED, release=LV_EVENT_RELEASED;
    board.panel_sleep=panel_sleep;
    (void)s_sleep_fade_timer;
    /* Both labels move as one block, every minute, in two dimensions. */
    int previous_x=0, previous_y=0;
    for (int minute=0; minute<8; minute++) {
        position_clock(minute*60000000LL);
        assert(cd.x==ct.x && cd.y-ct.y==54);
        assert(ct.x>=-38 && ct.x<=38 && ct.y>=-62 && ct.y<=14);
        if (minute) assert(ct.x!=previous_x || ct.y!=previous_y);
        previous_x=ct.x; previous_y=ct.y;
        position_clock(minute*60000000LL+59999999);
        assert(ct.x==previous_x && ct.y==previous_y);
    }
    s_w=s_h=128; s_small=true; position_clock(0);
    assert(ct.x>=-10 && cd.x<=10 && cd.y-ct.y==34);
    s_w=s_h=466; s_small=false;
    update_sleep(); assert(brightness==80);
    enabled=true; now=100; muse_state_set_asleep(true);
    now+=50000; /* delay before the UI task observes sleep must not reset the deadline */
    assert(update_sleep()); assert(brightness==80 && !panel);
    assert(s_sleep_fade==SLEEP_FADE_SCREEN_OUT && cover.opa>0 && cover.opa<255);
    assert(!muse_ui_dark());
    now=state_sleep_started+150000; update_sleep(); assert(cover.opa>0 && cover.opa<255);
    now=state_sleep_started+300000; update_sleep(); assert(cover.opa==255 && brightness==0);
    now=state_sleep_started+1999999; update_sleep(); assert(!s_clock_lit && brightness==0);
    now++; update_sleep(); assert(s_clock_lit && brightness==80 && !panel);
    assert(repaints==1 && cover.opa==255 && s_clock_opacity==0);
    assert(!ct.hidden && !cd.hidden && !strcmp(ct.text,"2:23"));
    now+=150000; update_sleep(); assert(s_clock_opacity>0 && s_clock_opacity<255);
    now+=150000; update_sleep(); assert(s_clock_opacity==255 && s_sleep_fade==SLEEP_FADE_NONE);
    on_cover_event(&press); update_sleep(); assert(s_sleep_fade==SLEEP_FADE_CLOCK_OUT);
    on_cover_event(&release); assert(!cover.hidden);
    now+=150000; update_sleep(); assert(s_clock_opacity>0 && s_clock_opacity<255 && cover.opa==255);
    now+=150000; update_sleep(); assert(ct.hidden && cd.hidden && s_sleep_fade==SLEEP_FADE_SCREEN_IN);
    now+=150000; update_sleep(); assert(cover.opa>0 && cover.opa<255);
    on_cover_event(&press); update_sleep(); /* extra taps cannot skip the screen fade */
    assert(s_sleep_fade==SLEEP_FADE_SCREEN_IN && cover.opa>0 && !cover.hidden);
    on_cover_event(&release); assert(!cover.hidden && !asleep && !s_dark);
    now+=150000; update_sleep(); assert(cover.hidden && s_sleep_fade==SLEEP_FADE_NONE);
    now++; muse_state_set_asleep(true); update_sleep();
    now+=2100000; update_sleep(); assert(s_sleep_fade==SLEEP_FADE_CLOCK_IN);
    on_cover_event(&press); update_sleep(); /* wake during entrance still fades out normally */
    assert(!asleep && !s_dark && !ct.hidden && !cd.hidden && cover.opa==255);
    assert(s_sleep_fade==SLEEP_FADE_CLOCK_OUT);
    on_cover_event(&release); assert(!cover.hidden);
    now+=600000; update_sleep(); assert(cover.hidden && ct.hidden && cd.hidden);
    now++; muse_state_set_asleep(true); update_sleep(); now+=2300000; update_sleep();
    muse_state_set_asleep(false); update_sleep(); assert(s_sleep_fade==SLEEP_FADE_CLOCK_OUT);
    on_cover_event(&press); update_sleep(); assert(s_sleep_fade==SLEEP_FADE_CLOCK_OUT);
    on_cover_event(&release); assert(!cover.hidden);
    now+=600000; update_sleep(); assert(cover.hidden);
    /* An uninterrupted wake finishes both phases and restores the screen. */
    now++; muse_state_set_asleep(true); update_sleep(); now+=2300000; update_sleep();
    muse_state_set_asleep(false); update_sleep(); now+=600000; update_sleep();
    assert(s_sleep_fade==SLEEP_FADE_NONE && cover.hidden && ct.hidden && brightness==80);
    /* A wake during lock fade reverses smoothly; it never jumps to clear. */
    now++; muse_state_set_asleep(true); update_sleep(); now+=150000; update_sleep();
    assert(s_sleep_fade==SLEEP_FADE_SCREEN_OUT);
    int midway_opacity=cover.opa;
    on_cover_event(&press); update_sleep(); assert(!asleep && !s_dark && cover.opa==midway_opacity);
    assert(s_sleep_fade==SLEEP_FADE_SCREEN_IN);
    on_cover_event(&release); assert(!cover.hidden);
    now+=300000; update_sleep(); assert(cover.hidden);
    enabled=false; now++; muse_state_set_asleep(true); update_sleep();
    assert(!panel && !muse_ui_dark() && brightness==80);
    now+=300000; update_sleep();
    assert(panel && muse_ui_dark() && brightness==0);
    muse_state_set_asleep(false); update_sleep(); assert(!panel && !muse_ui_dark());
    now+=300000; update_sleep(); assert(cover.hidden);
    /* AMOLED fades use only panel brightness between the endpoint redraws. */
    board.brightness_fade=true; enabled=true;
    now++; muse_state_set_asleep(true); update_sleep();
    assert(cover.opa==0 && brightness==80);
    now+=150000; update_sleep();
    assert(cover.opa==0 && brightness>0 && brightness<80);
    now+=150000; update_sleep(); assert(cover.opa==255 && brightness==0);
    now=state_sleep_started+2300000; update_sleep(); assert(s_clock_lit);
    muse_state_set_asleep(false); update_sleep();
    now+=300000; update_sleep();
    assert(s_sleep_fade==SLEEP_FADE_SCREEN_IN && cover.opa==0 && brightness==0);
    int distinct=0, previous_brightness=-1;
    for(int i=0;i<19;i++) {
        now+=16000; update_sleep();
        if(brightness!=previous_brightness) distinct++;
        previous_brightness=brightness;
        assert(cover.opa==0);
    }
    assert(distinct>=17 && brightness==80 && s_sleep_fade==SLEEP_FADE_NONE && cover.hidden);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            c = Path(tmp) / "sleep.c"
            exe = Path(tmp) / "sleep"
            c.write_text(code)
            subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_clock_rollover_and_offline_ticks(self):
        source = (ROOT / "components/muse/muse_clock.c").read_text()
        code = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
static bool s_valid;
static int64_t s_local_epoch, s_synced_us, now;
static int64_t esp_timer_get_time(void) { return now; }
'''
        code += function(source, "bool muse_clock_format(")
        code += r'''
int main(void) {
    char clock[16], date[40];
    assert(!muse_clock_format(clock,sizeof(clock),date,sizeof(date)));
    s_valid=true;
    s_local_epoch=1709164740; /* Feb 28, 2024, 23:59:00 local epoch */
    s_synced_us=now=500000;
    assert(muse_clock_format(clock,sizeof(clock),date,sizeof(date)));
    assert(!strcmp(clock,"11:59") && !strcmp(date,"Wed Feb 28"));
    now+=60000000;
    assert(muse_clock_format(clock,sizeof(clock),date,sizeof(date)));
    assert(!strcmp(clock,"12:00") && !strcmp(date,"Thu Feb 29"));
    now+=86400000000LL; /* still advances without a network refresh */
    assert(muse_clock_format(clock,sizeof(clock),date,sizeof(date)));
    assert(!strcmp(clock,"12:00") && !strcmp(date,"Fri Mar 1"));
    now+=14LL*3600*1000000+23LL*60*1000000;
    assert(muse_clock_format(clock,sizeof(clock),date,sizeof(date)));
    assert(!strcmp(clock,"2:23"));
    s_local_epoch=1709164740 + 60 - 12*3600; /* noon */
    s_synced_us=now;
    assert(muse_clock_format(clock,sizeof(clock),date,sizeof(date)));
    assert(!strcmp(clock,"12:00"));
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            c = Path(tmp) / "clock.c"
            exe = Path(tmp) / "clock"
            c.write_text(code)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
