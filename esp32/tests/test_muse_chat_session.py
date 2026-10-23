# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Exercise production PSRAM reply handlers with host-side event sinks."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
JSON = Path(os.environ.get(
    'CJSON_SOURCE_DIR', ROOT / 'managed_components/espressif__cjson/cJSON'
))


class ChatSession(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        out = Path(cls.tmp.name)
        source = (ROOT / 'components/muse/muse_chat_session.cpp').read_text()
        constants = source[source.index('#define MIC_RATE'):source.index('/* ---- Voice task')]
        types = source[source.index('enum phase_t'):source.index('/* 10 KB')]
        handlers = source[source.index('static int find_msg('):source.index('static void on_chat_ack(')]
        speech = source[source.index('static void start_tts(void)'):source.index('static void tts_data(')]
        caption = source[source.index('extern "C" bool muse_hatch_turn_caption('):
                         source.index('extern "C" size_t muse_hatch_turn_reply_text(')]
        deadlines = source[source.index('static void check_turn(void)'):source.index('/* ---- Inbound dispatch ---- */')]
        reset = source[source.index('static bool turn_start('):source.index('static void turn_begin(')]
        reply_text = source[source.index('extern "C" size_t muse_hatch_turn_reply_text('):
                            source.index('extern "C" size_t muse_hatch_turn_read(')]
        code = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include "host_compat.h"
#include "cJSON.h"
#include "minimp3.h"
#include "muse_chat_priv.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
''' + constants + types + r'''
static turn_t s_turn;
static char s_reply_shown[EV_TEXT];
static int64_t s_last_seq, s_marks[4];
static int captions, console_events;
static size_t caption_at;
enum mark_t { M_TEXT, M_DONE };
static void mark(mark_t) {}
static int64_t clock_us = 12345;
static int64_t now_us() { return clock_us; }
static int s_out;
static size_t output_room = 4096;
static size_t xStreamBufferSpacesAvailable(int) { return output_room; }
static void log_marks() {}
static void on_dictation_end(bool) {}
static void emit(muse_hatch_ev_t type, const char *) {
    if (type == MUSE_HATCH_EV_REPLY) captions++;
}
void muse_hatch_console(const char *, const char *, const char *, ...) { console_events++; }
void muse_hatch_tail_words(const char *text, char *out, size_t cap) { strlcpy(out, text, cap); }
bool muse_hatch_caption_at(const char *text, size_t at, char *out, size_t cap) {
    caption_at = at;
    strlcpy(out, text, cap); return text[0];
}
static void turn_finish() { s_turn.phase = P_IDLE; }
static void turn_fail(const char *) { turn_finish(); }
static void turn_done(bool) { turn_finish(); }
static bool ensure_connected() { return true; }
bool muse_hatch_configured() { return true; }
static void resampler_init(resampler_t *, int, int) {}
''' + reset + handlers + speech + deadlines + caption + reply_text + r'''
static void begin(bool typed = false) {
    assert(turn_start(s_turn.gen + 1, typed));
    s_turn.phase = P_WAIT_REPLY;
    s_turn.acked = true;
    strlcpy(s_turn.user_ids[0], "note", sizeof(s_turn.user_ids[0]));
    strlcpy(s_turn.user_ids[1], "parent", sizeof(s_turn.user_ids[1]));
    captions = console_events = 0;
}
static void event(const char *kind, const char *id, const char *parent = "", const char *text = "") {
    cJSON *root = cJSON_CreateObject(), *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "event");
    cJSON_AddStringToObject(root, "event", kind);
    cJSON_AddItemToObject(root, "payload", payload);
    cJSON_AddStringToObject(payload, "message_id", id);
    if (parent[0]) cJSON_AddStringToObject(payload, "reply_to_message_id", parent);
    cJSON_AddStringToObject(payload, "text", text);
    cJSON_AddStringToObject(payload, "display_text", text);
    on_event(root);
    cJSON_Delete(root);
}
static void rejected_deltas() {
    for (bool typed : {false, true}) {
        begin(typed);
        event("delta.message_start", "other", "elsewhere");
        event("delta.text_append", "other", "", "Wrong reply");
        event("delta.message_done", "other");
        event("message.assistant", "other", "", "Wrong final");
        assert(!s_turn.nmsgs && !captions && !console_events);
        assert(!s_turn.last_content_us && !s_turn.last_event_us);
        event("delta.message_start", "reply", "note");
        event("delta.text_append", "reply", "", "Our reply");
        event("delta.message_done", "reply");
        assert(s_turn.nmsgs == 1 && s_turn.msgs[0].done && s_turn.msgs[0].len == 9);
        assert(typed ? console_events == 2 : captions == 1);
        begin(typed);
        event("message.assistant", "other", "", "New turn");
        assert(s_turn.nmsgs == 1 && s_turn.msgs[0].done);
    }
}
static void valid_parents() {
    begin();
    event("message.assistant", "first", "note", "First");
    event("message.assistant", "second", "parent", "Second");
    event("message.assistant", "third", "first", "Third");
    event("message.assistant", "fourth", "", "Live");
    assert(s_turn.nmsgs == 4);
    for (int i = 0; i < s_turn.nmsgs; i++) assert(s_turn.msgs[i].done);
    cJSON *payload = cJSON_Parse("{\"parent_message_id\":\"elsewhere\"}");
    assert(bind_msg("fallback", payload) == -1);
    cJSON_Delete(payload);
    event("message.assistant", "fallback", "", "Wrong final");
    assert(s_turn.nmsgs == 4);
}
static void bounded_rejections() {
    begin();
    event("delta.message_start", "reply", "note");
    char id[20];
    for (int i = 0; i < 9; i++) { /* exceed the eight rejected IDs retained per turn */
        snprintf(id, sizeof(id), "other%d", i);
        event("delta.message_start", id, "elsewhere");
    }
    event("message.assistant", "other0", "", "Wrong final");
    event("message.assistant", id, "", "Overflow final");
    assert(s_turn.nmsgs == 1 && !captions && !console_events);
    event("delta.text_append", "reply", "", "Our reply");
    event("delta.message_done", "reply");
    event("message.assistant", "second", "note", "Second");
    assert(s_turn.nmsgs == 2 && s_turn.msgs[0].done && s_turn.msgs[1].done);
}
static void replay_text() {
    begin();
    event("message.assistant", "first", "note", "First answer");
    event("message.assistant", "second", "note", "Second answer");
    char text[128];
    assert(muse_hatch_turn_reply_text(text, sizeof(text)) == strlen("First answer\n\nSecond answer"));
    assert(strcmp(text, "First answer\n\nSecond answer") == 0);
    assert(muse_hatch_turn_reply_text(text, 8) == 7);
    assert(strcmp(text, "First a") == 0);
}
static void early_speech() {
    static char textbuf[MAX_MSGS * TEXT_MAX];
    s_turn.texts = textbuf;
    begin();
    event("delta.message_start", "reply", "note");
    event("delta.text_append", "reply", "note", "The first sentence is ready. ");
    msg_t &m = s_turn.msgs[0];
    assert(!m.done && m.tts == TTS_QUEUED);
    start_tts();
    assert(m.tts == TTS_ACTIVE && m.nsegments == 1);
    assert(m.segments[0].text_end == strlen("The first sentence is ready. "));
    event("delta.text_append", "reply", "", "The second sentence arrives later. ");
    s_turn.pcm_out = 100;
    finish_tts_segment();
    assert(m.tts == TTS_QUEUED && m.tts_offset == m.segments[0].text_end);
    start_tts();
    assert(m.nsegments == 2 && m.segments[1].text_start == m.tts_offset);
    event("delta.message_done", "reply");
    s_turn.pcm_out = 200;
    finish_tts_segment();
    assert(m.tts == TTS_FINISHED && m.tts_offset == m.len);

    begin();
    event("delta.message_start", "reply", "note");
    event("delta.text_append", "reply", "note", "An incomplete thought");
    assert(s_turn.msgs[0].tts == TTS_NONE);
    event("delta.message_done", "reply");
    assert(s_turn.msgs[0].tts == TTS_QUEUED);

    begin();
    event("delta.message_start", "first", "note");
    event("delta.text_append", "first", "", "Still writing the first message");
    event("message.assistant", "second", "note", "A later message is complete.");
    assert(s_turn.msgs[1].tts == TTS_QUEUED);
    start_tts();
    assert(s_turn.tts_msg == -1); /* preserve message order */
    event("delta.message_done", "first");
    start_tts();
    assert(s_turn.tts_msg == 0);

    begin();
    event("delta.message_start", "reply", "note");
    char long_text[256] = {};
    for (int i = 0; i < 45; i++) strcat(long_text, "word ");
    event("delta.text_append", "reply", "", long_text);
    assert(!s_turn.msgs[0].done && s_turn.msgs[0].tts == TTS_QUEUED);
    size_t end = next_tts_end(s_turn.msgs[0]);
    assert(end >= 64 && end <= 180 && long_text[end - 1] == ' ');
}
static void segment_caption() {
    static char textbuf[MAX_MSGS * TEXT_MAX];
    s_turn.texts = textbuf;
    begin();
    strcpy(textbuf, "First. Second.");
    msg_t &m = s_turn.msgs[0];
    s_turn.nmsgs = 1;
    m.len = strlen(textbuf);
    m.nsegments = 2;
    m.segments[0] = {0, 7, 0, 100};
    m.segments[1] = {7, 14, 100, 200};
    char out[64];
    assert(muse_hatch_turn_caption(50, out, sizeof(out)) && caption_at == 3);
    assert(muse_hatch_turn_caption(150, out, sizeof(out)) && caption_at == 10);
}

static void long_reply(bool streaming) {
    static char textbuf[MAX_MSGS * TEXT_MAX];
    s_turn.texts = textbuf;
    std::string samples[3];
    for (int i = 0; i < 1600; i++) samples[0] += "Aha! Yes. ";
    samples[1] = std::string(TEXT_MAX - 1, 'z'); /* no word boundary before the limit */
    while (samples[2].size() + strlen("こんにちは世界🌍 ") < TEXT_MAX)
        samples[2] += "こんにちは世界🌍 ";
    for (const auto &full : samples) {
        begin();
        event("delta.message_start", "reply", "note");
        msg_t &m = s_turn.msgs[0];
        std::string spoken;
        auto drain = [&]() {
            while (m.tts == TTS_QUEUED) {
                assert(m.nsegments < TTS_SEGMENTS);
                start_tts();
                assert(m.tts == TTS_ACTIVE);
                const auto &seg = m.segments[m.nsegments - 1];
                assert(seg.text_end > seg.text_start);
                assert(seg.text_end - seg.text_start <= TTS_CHUNK_MAX);
                assert(seg.text_end == full.size() || ((unsigned char)full[seg.text_end] & 0xc0) != 0x80);
                spoken.append(full, seg.text_start, seg.text_end - seg.text_start);
                s_turn.pcm_out += 100;
                finish_tts_segment();
            }
        };
        if (streaming) {
            for (size_t off = 0; off < full.size(); off += 137) {
                event("delta.text_append", "reply", "", full.substr(off,137).c_str());
                drain();
            }
            event("delta.message_done", "reply");
        } else {
            event("message.assistant", "reply", "note", full.c_str());
        }
        drain();
        assert(m.len == full.size() && strlen(textbuf) == full.size());
        assert(m.tts == TTS_FINISHED && m.tts_offset == full.size());
        assert(spoken == full); /* every byte reaches a speech segment, including the tail */
    }
}
static void playback_deadline() {
    begin();
    s_turn.start_us = 1;
    s_turn.nmsgs = 1;
    s_turn.msgs[0].done = true;
    s_turn.msgs[0].tts = TTS_ACTIVE;
    clock_us = 2 * TURN_CAP_US;
    s_turn.last_audio_us = clock_us - 1000;
    s_turn.pcm_out = 100;
    check_turn();
    assert(s_turn.phase == P_WAIT_REPLY); /* long reply is still making progress */
    clock_us += TURN_CAP_US + 1;
    output_room = 0;
    check_turn();
    assert(s_turn.phase == P_WAIT_REPLY); /* paused/backpressured output isn't a stalled request */
    output_room = 4096;
    clock_us += TURN_CAP_US + 1;
    check_turn();
    assert(s_turn.phase == P_IDLE); /* no progress and no backpressure still times out */

    begin();
    s_turn.start_us = 1;
    s_turn.nmsgs = 0;
    clock_us += TURN_CAP_US;
    check_turn();
    assert(s_turn.phase == P_IDLE); /* no reply retains its deadline */
}

int main(int argc, char **argv) {
    assert(argc == 2);
    switch (atoi(argv[1])) {
    case 0: rejected_deltas(); break;
    case 1: valid_parents(); break;
    case 2: bounded_rejections(); break;
    case 3: replay_text(); break;
    case 4: early_speech(); break;
    case 5: segment_caption(); break;
    case 6: long_reply(false); break;
    case 7: long_reply(true); break;
    case 8: playback_deadline(); break;
    default: return 2;
    }
}
'''
        (out / 'session.cpp').write_text(code)
        flags = ['-Wall', '-Wextra', '-Werror', '-I', str(JSON),
                 '-I', str(ROOT / 'tests'), '-I', str(ROOT / 'components/muse'),
                 '-I', str(ROOT / 'components/minimp3/include')]
        commands = [
            [*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', *flags,
             '-c', str(JSON / 'cJSON.c'), '-o', str(out / 'cjson.o')],
            [*shlex.split(os.environ.get('CXX', 'c++')), '-std=gnu++17', *flags,
             str(out / 'session.cpp'), str(out / 'cjson.o'), '-o', str(out / 'session')],
        ]
        for command in commands:
            result = subprocess.run(command, capture_output=True, text=True)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
        cls.binary = out / 'session'

    def run_case(self, case):
        result = subprocess.run([str(self.binary), str(case)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_rejected_deltas_do_not_emit_or_complete_voice_or_typed_replies(self):
        self.run_case(0)

    def test_ack_parent_reply_chain_and_parentless_messages_still_work(self):
        self.run_case(1)

    def test_rejection_capacity_preserves_correlation(self):
        self.run_case(2)

    def test_replay_assembles_full_completed_reply(self):
        self.run_case(3)

    def test_speech_starts_at_stable_text_and_drains_later_segments(self):
        self.run_case(4)

    def test_caption_tracks_the_audio_segment_being_played(self):
        self.run_case(5)

    def test_long_replies_preserve_all_text_in_bounded_utf8_speech_chunks(self):
        self.run_case(6)

    def test_long_streamed_replies_cannot_exhaust_speech_segment_slots(self):
        self.run_case(7)

    def test_playback_progress_and_pause_extend_voice_deadline(self):
        self.run_case(8)
