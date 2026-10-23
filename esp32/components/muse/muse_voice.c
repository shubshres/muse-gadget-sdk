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

#include "muse_voice.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "muse_adpcm.h"
#include "muse_audio.h"
#include "muse_board.h"
#include "muse_chat.h"
#include "muse_input.h"
#include "muse_mem.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_wifi.h"
#include "muse_voice_limits.h"

static const char *TAG = "muse_voice";

/* Pick once per reply so acknowledgements don't shuffle the caption. */
static const char *thinking_caption(void)
{
static const char *const captions[] = {
        /* --- Original List --- */
        "WARMING UP MY BRAIN...",
        "SHAKING THE IDEA JAR...",
        "ASKING MY INNER OWL...",
        "UNTANGLING THOUGHTS...",
        "GIVING THAT A NOODLE...",
        "BRB, BRAIN GYM...",
        "ADDING A PINCH OF GENIUS...",
        "HUNTING FOR A LIGHTBULB...",
        "HERDING MY BRAIN CELLS...",
        "SPINNING THE THOUGHT WHEEL...",
        "THINKING CAP: ACTIVATED!",
        "COOKING UP AN ANSWER...",
        "LET ME COOK! LET ME COOK NOW! LET ME COOK! I SAID LET ME COOK!...",

        /* --- Internet Culture & Memes --- */
        "CONSULTING THE LORE...",
        "SUMMONING THE VIBE SHIFT...",
        "DOWNLOADING MORE RAM...",
        "LOADING... PLEASE DO NOT TURN OFF YOUR COMPUTER...",
        "BUFFERING AT 99%...",
        "DELETING SYSTEM32 FOR MAXIMUM SPEED...",
        "THE HAMSTER IS RUNNING AS FAST AS HE CAN...",
        "CHARGING THE SINGLE SHARED BRAINCELL...",
        "QUEUEING UP THE DIAL-UP NOISE...",
        "CONSULTING THE COUNCIL OF DOGES...",
        "BRAIN IS GIVING '404 NOT FOUND'...",
        "RUNNING ON ICED COFFEE AND PURE ANXIETY...",
        "ACTIVATING 'VERY DEMURE, VERY MINIMALIST' THOUGHTS...",
        "BETA TESTING A GOOD IDEA...",
        "PERFORMING A MENTAL SPEEDRUN...",
        "TOUCHING GRASS TO UNLOCK KNOWLEDGE...",
        "AURA +1000 WHILE I PONDER...",
        "MAIN CHARACTER MOMENT LOADING...",
        "WAITING FOR THE ALGORITHM TO DECIDE...",
        "THE CELESTIAL JUKEBOX IS SPINNING...",

        /* --- Gen Z / TikTok Slang --- */
        "LOCKED IN. DO NOT DISTURB...",
        "COOKING A GOURMET MEAL IN MY MIND...",
        "SIMMERING ON THAT...",
        "YAPPIN' TO MYSELF...",
        "MANIFESTING THE CORRECT ANSWER...",
        "IN MY DEEP THOUGHT ERA...",
        "SENDING TELEPATHIC PING...",
        "PROCESSING AT 3AM THOUGHT SPEEDS...",
        "VIBING WITH THE VOID...",
        "MAKING IT MAKE SENSE...",
        "FS IN THE CHAT FOR MY LAST BRAINCELL...",
        "ABSOLUTELY NO THOUGHTS, JUST ELEVATOR MUSIC... OK WAIT, GOT ONE...",

        /* --- Pop Culture & Current Events Vibe --- */
        "CONSULTING CHATGPT ABOUT CHATGPT...",
        "CHECKING THE STOCK MARKET OF MY IDEAS...",
        "PULLING UP THE WIKIPEDIA RABBIT HOLE...",
        "NAVIGATING THE MULTIVERSE OF POSSIBILITIES...",
        "DECODING THE MATRIX...",
        "CONSULTING THE TIME TRAVELERS...",
        "WAITING FOR MARS COLONIZATION APPROVAL...",
        "ANALYZING THE QUANTUM FLUCTUATIONS...",
        "REBOOTING THE SIMULATION...",
        "CONSULTING THE ORACLE OF DELPHI...",
        "CHECKING MY HOROSCOPE FOR ANSWERS...",
        "ASKING THE PIGEONS OUTSIDE...",
        "CONSULTING THE COUNCIL OF CATS...",

        /* --- Pure Comedy & Relatable Chaos --- */
        "HOLD ON, LET ME OVERTHINK THIS...",
        "MASHING THE KEYBOARD IN THOUGHT...",
        "SEARCHING FOR THAT ONE MEMORY FROM 2012...",
        "BLOWING INTO THE CARTRIDGE...",
        "SHAKING THE SNOWGLOBE OF MY MIND...",
        "STARING BLANKLY INTO THE MIDDLE DISTANCE...",
        "WAITING FOR INSPIRATION TO STRIKE (LIKE LIGHTNING)...",
        "TURNING IT OFF AND ON AGAIN...",
        "PASSING THE THOUGHT TO THE NEXT DEPARTMENT...",
        "WRITING A STRONGLY WORDED MENTAL LETTER...",
        "CONSULTING MY ANCESTORS...",
        "GATHERING THE SAGE AND CRYSTALS...",
        "RUNNING THE NUMBERS (THEY ARE FAKE)...",
        "PERFORMING AN EXORCISM ON A STUPID IDEA...",
        "STUCK IN THE THOUGHT DRIVE-THRU..."
    };
    return captions[esp_random() % (sizeof(captions) / sizeof(captions[0]))];
}


#define MAX_SECS MUSE_VOICE_RECORD_MAX_SECONDS
#define TAIL_FRAMES (MUSE_AUDIO_RATE * 12 / 100)   /* capture lag + poll interval, stops before the release click */
#define MAX_FRAMES (MUSE_AUDIO_RATE * MAX_SECS)
#define MIN_HELD_FRAMES (MUSE_AUDIO_RATE * 3 / 10)   /* shorter presses are taps, not speech */
#if CONFIG_MUSE_BOARD_M5STACK_CARDPUTER_ADV || CONFIG_MUSE_BOARD_AI_PASSPORT
#define PRE_CHUNKS 6                                   /* 120 ms; a 320 ms upload burst stalls on the ADV and C3 */
#else
#define PRE_CHUNKS 16                                  /* 320 ms of audio kept from before the press */
#endif
#define SETTLE_CHUNKS 10   /* after Muse makes a sound, 200 ms of capture is its own tail */
#define REST_BACKSTOP_MS 60000

#if CONFIG_MUSE_HATCH
/* A note recorded while Hatch is out of reach is saved in PSRAM, and goes once it's back. */
#define HOLD_NOTES 1
#else
#define HOLD_NOTES 0   /* no PSRAM: a press needs Hatch */
#endif
#define HELD_MAX 4
#define HELD_TRIES 8                            /* then a saved note is dropped */
#define HELD_POLL_MS 5000                       /* resting with notes saved: check for Wi-Fi this often */
#define HELD_KEEP_US (30LL * 60 * 1000000)      /* muse_voice_notes_waiting() */
#define RETRY_MIN_US (15LL * 1000000)
#define RETRY_MAX_US (120LL * 1000000)
#define STALL_US (30LL * 1000000)               /* Hatch took none of a note this long: give up */
#define ACK_WAIT_US (30LL * 1000000)            /* asleep, waiting for the VM to have a note */

static QueueHandle_t s_queue;
static atomic_bool s_cancel_response;
static atomic_bool s_power_off_requested;
static atomic_bool s_pause_response;
static atomic_bool s_replay_request;
static atomic_bool s_replaying;
static atomic_bool s_last_available;
static SemaphoreHandle_t s_last_lock;
static char *s_last_text;
static int16_t *s_last_pcm;   /* used and replaced only by the voice task */
static size_t s_last_frames;

#define REPLAY_MAX_FRAMES (MUSE_AUDIO_RATE * 45)
#define REPLAY_GROW_FRAMES (MUSE_AUDIO_RATE * 2)

bool muse_voice_has_last_answer(void)
{
    return atomic_load(&s_last_available);
}

size_t muse_voice_copy_last_answer(char *out, size_t cap)
{
    if (!out || !cap || !s_last_lock) return 0;
    xSemaphoreTake(s_last_lock, portMAX_DELAY);
    strlcpy(out, s_last_text ? s_last_text : "", cap);
    size_t n = strlen(out);
    xSemaphoreGive(s_last_lock);
    return n;
}

bool muse_voice_replay_last(void)
{
    if (!muse_voice_has_last_answer() || atomic_load(&s_replaying)) return false;
    atomic_store(&s_replay_request, true);
    muse_state_nudge();
    return true;
}

bool muse_voice_replaying(void)
{
    return atomic_load(&s_replaying);
}

static void save_last_answer(int16_t *pcm, size_t frames)
{
    size_t cap = MUSE_LOW_MEM ? 1024 : MUSE_REPLAY_TEXT_CAP;
    char *text = heap_caps_malloc(cap, MUSE_BIG_CAPS);
    if (!text) {
        free(pcm);
        return;
    }
    char prompt[512];
    size_t have = muse_hatch_turn_prompt_text(prompt, sizeof(prompt));
    size_t used = have ? (size_t)snprintf(text, cap, "You:\n%s\n\nReply:\n", prompt) : 0;
    if (used >= cap) used = cap - 1;
    size_t reply = muse_hatch_turn_reply_text(text + used, cap - used);
    if (!have && !reply) {
        free(text);
        free(pcm);
        return;
    }
    if (have && !reply) strlcat(text, "Response stopped before a reply arrived.", cap);
    xSemaphoreTake(s_last_lock, portMAX_DELAY);
    free(s_last_text);
    free(s_last_pcm);
    s_last_text = text;
    s_last_pcm = pcm;
    s_last_frames = frames;
    atomic_store(&s_last_available, true);
    xSemaphoreGive(s_last_lock);
}

void muse_voice_cancel_response(void)
{
    atomic_store(&s_replay_request, false);
    atomic_store(&s_pause_response, false);
    atomic_store(&s_cancel_response, true);
}

bool muse_voice_paused(void)
{
    return atomic_load(&s_pause_response);
}

void muse_voice_toggle_pause(void)
{
    if (muse_state_mode(NULL) == MUSE_MODE_SPEAKING) {
        atomic_store(&s_pause_response, !atomic_load(&s_pause_response));
    }
}

static volatile bool s_monitor;
static volatile float s_monitor_db = -100.0f;
static volatile bool s_chirp;
static volatile bool s_loopback;
static volatile bool s_mp3test;

/*
 * Pre-roll: while idle the mic keeps running into this ring, so a recording
 * can start a little before the press. People start talking as they press,
 * and the power key reports the press late.
 */
typedef struct {
#if MUSE_LOW_MEM
    muse_adpcm_t start;   /* encoder state before this chunk, to decode it alone */
    uint8_t adpcm[MUSE_AUDIO_CHUNK / 2];
#else
    int16_t pcm[MUSE_AUDIO_CHUNK];
#endif
} pre_chunk_t;

static pre_chunk_t *s_pre;   /* PRE_CHUNKS, a ring */
static size_t s_pre_next, s_pre_fill;
#if MUSE_LOW_MEM
static muse_adpcm_t s_pre_enc;
#endif
static int16_t s_chunk[MUSE_AUDIO_CHUNK];
static int s_settle;

/*
 * The note being recorded (HOLD_NOTES), kept whole in case it can't go now.
 * Streaming, Hatch has had s_sent frames of it.
 */
static int16_t *s_rec;   /* MAX_FRAMES */
static size_t s_rec_n, s_sent;
static bool s_live = true;
static bool s_tried;     /* it went to Hatch */

typedef struct {
    int16_t *pcm;
    size_t frames;
    int64_t at_us;   /* when it was recorded */
    int tries;
} held_note_t;

static int s_held_count;
static volatile bool s_waiting;        /* muse_voice_notes_waiting() */
#if HOLD_NOTES
static held_note_t s_held[HELD_MAX];   /* oldest first */
static int64_t s_next_send_us;
static int64_t s_send_backoff_us = RETRY_MIN_US;
#endif

/* The i-th oldest pre-roll chunk, into out. */
static void pre_get(size_t i, int16_t *out)
{
    const pre_chunk_t *c = &s_pre[(s_pre_next + PRE_CHUNKS - s_pre_fill + i) % PRE_CHUNKS];
#if MUSE_LOW_MEM
    muse_adpcm_t dec = c->start;
    muse_adpcm_decode_block(&dec, c->adpcm, MUSE_AUDIO_CHUNK, out);
#else
    memcpy(out, c->pcm, sizeof(c->pcm));
#endif
}

/* Call after Muse plays anything: its own sound must not become pre-roll. */
static void pre_reset(void)
{
    s_pre_fill = 0;
    s_settle = SETTLE_CHUNKS;
}

/* One 20 ms idle read: feeds the pre-roll ring and the settings mic meter. */
static void idle_capture(void)
{
    if (muse_audio_read(s_chunk, MUSE_AUDIO_CHUNK) != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(20));
        return;
    }
    if (s_monitor) {
        float db = muse_audio_dbfs(s_chunk, MUSE_AUDIO_CHUNK);
        s_monitor_db = db > s_monitor_db ? db : s_monitor_db * 0.9f + db * 0.1f;
    }
    if (s_settle > 0) {
        s_settle--;
        return;
    }
    pre_chunk_t *c = &s_pre[s_pre_next];
#if MUSE_LOW_MEM
    c->start = s_pre_enc;
    muse_adpcm_encode_block(&s_pre_enc, s_chunk, MUSE_AUDIO_CHUNK, c->adpcm);
#else
    memcpy(c->pcm, s_chunk, sizeof(c->pcm));
#endif
    s_pre_next = (s_pre_next + 1) % PRE_CHUNKS;
    s_pre_fill = s_pre_fill < PRE_CHUNKS ? s_pre_fill + 1 : PRE_CHUNKS;
}

/* Non-blocking: returns true if an event of `type` arrived (others dropped). */
static bool got_event(muse_ptt_t type)
{
    muse_input_event_t ev;
    bool hit = false;
    while (xQueueReceive(s_queue, &ev, 0) == pdTRUE) {
        muse_state_poke();
        hit |= ev.type == type;
    }
    return hit;
}

/* Level stats of a recording, gathered chunk by chunk for the log. */
typedef struct {
    double acc;
    size_t frames;
    int peak, clipped;
    float floor_db;
    float tail_db[25];   /* the last 0.5 s, to see where speech ends and the release click lands */
    size_t chunks;
} rec_stats_t;

/* Adds one captured chunk (up to MUSE_AUDIO_CHUNK frames) to the stats. */
static void keep(rec_stats_t *st, const int16_t *pcm, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int v = abs(pcm[i]);
        st->peak = v > st->peak ? v : st->peak;
        st->clipped += v >= 32000;
        st->acc += (double)pcm[i] * pcm[i];
    }
    st->frames += n;
    if (n == MUSE_AUDIO_CHUNK) {
        float db = muse_audio_dbfs(pcm, n);
        st->floor_db = db < st->floor_db ? db : st->floor_db;
        st->tail_db[st->chunks++ % 25] = db;
    }
}

/* Gives Hatch what it has room for of the note being recorded. */
static void feed_live(void)
{
#if HOLD_NOTES
    if (s_live && s_sent < s_rec_n) {
        s_sent += muse_hatch_turn_audio_wait(s_rec + s_sent, s_rec_n - s_sent, 0);
    }
#endif
}

/* Streams the note being recorded from its start, then the rest as it comes. */
static void go_live(void)
{
    muse_hatch_turn_begin();
    s_live = s_tried = true;
    s_sent = 0;
    feed_live();
}

/* One chunk of speech: into the stats, and on to Hatch, by way of the kept note if there is one. */
static void take(rec_stats_t *st, const int16_t *pcm)
{
    keep(st, pcm, MUSE_AUDIO_CHUNK);
    if (!s_rec) {
        if (s_live) {
            muse_hatch_turn_audio(pcm, MUSE_AUDIO_CHUNK);
        }
        return;
    }
    memcpy(s_rec + s_rec_n, pcm, MUSE_AUDIO_CHUNK * sizeof(int16_t));
    s_rec_n += MUSE_AUDIO_CHUNK;
    feed_live();
}

/*
 * Records speech until release or MAX_SECS, starting with the pre-roll; *held
 * gets the part after the press. It streams to Hatch as it goes if it can.
 * If not, the note is kept in s_rec to send later, and streams from partway
 * if Hatch comes within reach. Returns false if Hatch failed the turn with no
 * kept note to fall back on (why says what failed).
 */
static bool record(size_t *held, char *why, size_t cap)
{
    muse_state_set_mode(MUSE_MODE_LISTENING);
    muse_state_set_progress(0);
    s_rec_n = s_sent = 0;
    s_live = s_tried = false;
    if (!s_rec || (muse_hatch_ready() && !s_held_count)) {
        go_live();
    }
    muse_state_set_caption(s_live ? "LISTENING..." : "RECORDING...");
    bool heard = false, ok = true;
    bool gave_up = false;   /* Hatch failed this note: it's kept, and goes later */
    char text[96];
    rec_stats_t st = { 0 };

    size_t n = 0;
    for (size_t i = 0; i < s_pre_fill; i++) {
        pre_get(i, s_chunk);
        take(&st, s_chunk);
        n += MUSE_AUDIO_CHUNK;
    }
    size_t pre = n;
    bool released = false;
    size_t stop_at = MAX_FRAMES;
    while (n + MUSE_AUDIO_CHUNK <= stop_at && !atomic_load(&s_power_off_requested)) {
        if (muse_audio_read(s_chunk, MUSE_AUDIO_CHUNK) != ESP_OK) {
            break;
        }
        muse_state_set_level(muse_audio_level(s_chunk, MUSE_AUDIO_CHUNK));
        take(&st, s_chunk);
        /* Live transcript as the caption. A failure stops the streaming; the
         * kept note goes later, or without one the failure is the caption. */
        muse_hatch_ev_t ev;
        while (s_live && (ev = muse_hatch_turn_event(text, sizeof(text))) != MUSE_HATCH_EV_NONE) {
            if (ev == MUSE_HATCH_EV_HEARD && text[0]) {
                heard = true;
                muse_state_set_caption("%s", text);
            } else if (ev == MUSE_HATCH_EV_ERROR) {
                ESP_LOGW(TAG, "muse: %s", text);
                muse_hatch_turn_cancel();
                s_live = false;
                gave_up = true;
                if (!s_rec) {
                    ok = false;
                    strlcpy(why, text, cap);
                    muse_state_set_caption("%s", text);
                }
            }
        }
        n += MUSE_AUDIO_CHUNK;
        bool tick = n % (MUSE_AUDIO_CHUNK * 5) == 0;
        if (tick && s_rec && !s_live && !gave_up && !s_held_count && muse_hatch_ready()) {
            ESP_LOGI(TAG, "Muse in reach: streaming the note so far");
            go_live();
        }
        muse_state_set_progress((float)n / MAX_FRAMES);
        if (!heard && ok && tick) {
            muse_state_set_caption("%s %.1fs", s_live ? "LISTENING" : "RECORDING", (double)n / MUSE_AUDIO_RATE);
        }
        /*
         * Capture runs 60-80 ms behind real time and people let go on their
         * last syllable, so keep going briefly after release.
         */
        if (!released && got_event(MUSE_PTT_UP)) {
            released = true;
            stop_at = n + TAIL_FRAMES < MAX_FRAMES ? n + TAIL_FRAMES : MAX_FRAMES;
        }
    }
    muse_state_set_level(0);
    *held = n - pre;

    char tail[160];
    int tl = 0;
    size_t shown = st.chunks < 25 ? st.chunks : 25;
    for (size_t i = st.chunks - shown; i < st.chunks; i++) {
        tl += snprintf(tail + tl, sizeof(tail) - tl, " %.0f", st.tail_db[i % 25]);
    }
    ESP_LOGI(TAG, "end levels:%s", tail);
    float rms_db = st.frames ? 10.0f * log10f((float)(st.acc / st.frames / (32768.0 * 32768.0)) + 1e-10f) : -100.0f;
    ESP_LOGI(TAG, "recorded %.2fs%s: rms %.1f dBFS, peak %.1f dBFS, floor %.1f dBFS, %d clipped, gain %d dB",
             (double)n / MUSE_AUDIO_RATE, released ? "" : " (max)", rms_db < -100.0f ? -100.0f : rms_db,
             20.0 * log10((st.peak + 1) / 32768.0), st.floor_db, st.clipped, muse_settings_mic_gain());
    return ok;
}

static void go_idle(const char *caption);

/*
 * Plays Hatch's reply as it arrives, with its text as the caption. Returns
 * true if interrupted by a new press. *delivered: the VM has the note.
 */
static bool hatch_reply(bool *delivered)
{
    atomic_store(&s_pause_response, false);
    muse_state_set_mode(MUSE_MODE_THINKING);
    const char *waiting = thinking_caption();
    muse_state_set_caption("%s", waiting);   /* until there's a transcript or reply */
    static int16_t buf[MUSE_AUDIO_CHUNK];
    static const int16_t silence[MUSE_AUDIO_CHUNK];
    char text[96];
    static char page[MUSE_CAPTION_MAX];
    bool done = false, speaking = false, replied = false;
    bool pause_caption = false;
    size_t played = 0;
    int16_t *replay_pcm = NULL;
    size_t replay_frames = 0, replay_capacity = 0;
    bool replay_overflow = false;
    int64_t t0 = esp_timer_get_time();
    *delivered = false;
    for (;;) {
        if (atomic_exchange(&s_cancel_response, false)) {
            ESP_LOGI(TAG, "reply stopped by screen gesture");
            save_last_answer(replay_pcm, replay_frames);
            replay_pcm = NULL;
            muse_hatch_turn_cancel();
            *delivered = true;   /* an intentional stop must not resend a saved note */
            muse_state_set_level(0);
            go_idle("STOPPED");
            free(replay_pcm);
            return false;
        }
        muse_hatch_ev_t ev;
        while ((ev = muse_hatch_turn_event(text, sizeof(text))) != MUSE_HATCH_EV_NONE) {
            switch (ev) {
            case MUSE_HATCH_EV_HEARD:
                if (!speaking && !replied) {
                    muse_state_set_caption("\"%s\"", text);
                }
                break;
            case MUSE_HATCH_EV_SENT:
                *delivered = true;
                if (!speaking && !replied) {
                    muse_state_set_caption("%s", waiting);
                }
                break;
            case MUSE_HATCH_EV_REPLY:
                replied = *delivered = true;
                /* Once speech starts, the caption follows it. The event only
                 * has room for the page's start; the page itself comes below. */
                if (!speaking && !muse_hatch_turn_caption(played, page, sizeof(page))) {
                    muse_state_set_caption("%s", text);
                }
                break;
            case MUSE_HATCH_EV_DONE:
                done = *delivered = true;
                break;
            case MUSE_HATCH_EV_ERROR:
                ESP_LOGW(TAG, "muse: %s", text);
                muse_state_set_level(0);
                go_idle(text);
                free(replay_pcm);
                return false;
            default:
                break;
            }
        }
        if (got_event(MUSE_PTT_DOWN)) {
            ESP_LOGI(TAG, "reply interrupted");
            save_last_answer(replay_pcm, replay_frames);
            replay_pcm = NULL;
            muse_hatch_turn_cancel();
            muse_state_set_level(0);
            free(replay_pcm);
            return true;
        }
        if (atomic_load(&s_pause_response)) {
            muse_state_set_level(0);
            if (!pause_caption) {
                muse_state_set_caption("PAUSED - TAP MUSE TO RESUME");
                pause_caption = true;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;   /* leave streamed audio in Hatch's buffer until resumed */
        }
        if (pause_caption) {
            pause_caption = false;
            if (muse_hatch_turn_caption(played, page, sizeof(page))) {
                muse_state_set_caption("%s", page);
            } else {
                muse_state_set_caption("SPEAKING");
            }
        }
        size_t n = muse_hatch_turn_read(buf, MUSE_AUDIO_CHUNK, speaking || done ? 0 : 20);
        if (atomic_load(&s_cancel_response)) continue;
        if (n) {
            if (!speaking) {
                speaking = true;
                muse_state_set_mode(MUSE_MODE_SPEAKING);
                ESP_LOGI(TAG, "reply audio after %.2fs", (esp_timer_get_time() - t0) / 1e6);
            }
            muse_state_set_level(muse_audio_level(buf, n));
            muse_audio_write(buf, n);
            played += n;
#if !MUSE_LOW_MEM
            if (!replay_overflow) {
                if (replay_frames + n > REPLAY_MAX_FRAMES) {
                    replay_overflow = true;
                } else if (replay_frames + n > replay_capacity) {
                    size_t next = replay_capacity + REPLAY_GROW_FRAMES;
                    int16_t *larger = heap_caps_realloc(replay_pcm, next * sizeof(int16_t), MUSE_BIG_CAPS);
                    if (!larger) replay_overflow = true;
                    else {
                        replay_pcm = larger;
                        replay_capacity = next;
                    }
                }
                if (!replay_overflow) {
                    memcpy(replay_pcm + replay_frames, buf, n * sizeof(int16_t));
                    replay_frames += n;
                }
                if (replay_overflow) {
                    free(replay_pcm);
                    replay_pcm = NULL;
                    replay_frames = 0;
                }
            }
#endif
        } else if (done) {
            break;
        } else if (speaking) {
            /* Between messages: keep the speaker fed so it doesn't replay stale DMA. */
            muse_state_set_level(0);
            muse_audio_write(silence, MUSE_AUDIO_CHUNK);
        }
        /* The page being said, or before the speech the reply's opening page. */
        if ((speaking || replied) && muse_hatch_turn_caption(played, page, sizeof(page))) {
            muse_state_set_caption("%s", page);
        }
    }
    muse_state_set_level(0);
    ESP_LOGI(TAG, "muse reply: %.2fs of audio, %.2fs total", (double)played / MUSE_AUDIO_RATE,
             (esp_timer_get_time() - t0) / 1e6);
    if (!played) {
        /* No speech (TTS unavailable): leave the reply text up for a moment. */
        for (int i = 0; i < 25 && !atomic_load(&s_cancel_response); i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (atomic_exchange(&s_cancel_response, false)) {
            muse_hatch_turn_cancel();
            *delivered = true;
            go_idle("STOPPED");
            free(replay_pcm);
            return false;
        }
    }
    save_last_answer(replay_pcm, replay_frames);
    return false;
}

static void go_idle(const char *caption)
{
    muse_state_set_progress(0);
    muse_state_set_mode(MUSE_MODE_IDLE);
    muse_state_set_caption("%s", caption);
}

/* Why a press can't go to Hatch; voice notes only go there. */
static const char *not_ready_reason(void)
{
    muse_hatch_status_t st;
    muse_hatch_status(&st);
    switch (st.state) {
    case MUSE_HATCH_NOT_SET: return "SET UP MUSE FIRST";
    case MUSE_HATCH_OFFLINE: return "NO WI-FI";
    default: return "CAN'T REACH MUSE";
    }
}

static volatile bool s_resting;

/*
 * Asleep on battery with nothing to do: power the codecs down (the loop rests
 * Wi-Fi alongside). They come back within one wait on waking, and a talk press
 * that woke Muse is kept for the loop (a note may follow). On USB power
 * nothing changes asleep.
 */
static void set_resting(bool rest)
{
    if (rest == s_resting) {
        return;
    }
    s_resting = rest;
    muse_audio_power(!rest);
    muse_hatch_set_resting(rest);
    if (rest) {
        /* Presses from before sleeping are stale, but not one that just woke Muse. */
        muse_input_event_t ev;
        while (xQueueReceive(s_queue, &ev, 0) == pdTRUE) {
            if (ev.wake) {
                xQueueSendToFront(s_queue, &ev, 0);
                break;
            }
        }
    } else {
        pre_reset();
    }
    ESP_LOGI(TAG, "%s", rest ? "resting: codecs off, Wi-Fi modem sleep" : "awake: codecs on");
}

/*
 * A press that woke the screen: listen from now, into the pre-roll, and
 * record only if it's still held after a tap's length. False if it was let
 * go sooner: it only woke Muse.
 */
static bool held_on_waking(void)
{
    s_settle = 0;   /* nothing played: no tail of Muse's own to skip */
    for (size_t n = 0; n < MIN_HELD_FRAMES; n += MUSE_AUDIO_CHUNK) {
        if (got_event(MUSE_PTT_UP)) {
            return false;
        }
        idle_capture();
    }
    return !got_event(MUSE_PTT_UP);
}

static void drop_rec(void)
{
    free(s_rec);
    s_rec = NULL;
}

#if HOLD_NOTES
/* True if a press is queued, left there for the loop; releases ahead of it are dropped. */
static bool press_waiting(void)
{
    muse_input_event_t ev;
    while (xQueuePeek(s_queue, &ev, 0) == pdTRUE) {
        if (ev.type == MUSE_PTT_DOWN) {
            return true;
        }
        xQueueReceive(s_queue, &ev, 0);
    }
    return false;
}

static void back_off(void)
{
    s_next_send_us = esp_timer_get_time() + s_send_backoff_us;
    s_send_backoff_us = s_send_backoff_us * 2 > RETRY_MAX_US ? RETRY_MAX_US : s_send_backoff_us * 2;
}

/* Saves the note just recorded to send later. `tried`: Hatch failed it, so it waits out the backoff. */
static void hold_rec(bool tried)
{
    if (s_held_count >= HELD_MAX) {   /* can_record() leaves room: not expected */
        drop_rec();
        go_idle("COULDN'T SAVE THE NOTE");
        return;
    }
    if (!s_rec_n) {   /* nothing captured: realloc to 0 would free s_rec and return NULL */
        drop_rec();
        go_idle("HOLD LONGER TO TALK");
        return;
    }
    int16_t *pcm = heap_caps_realloc(s_rec, s_rec_n * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_held[s_held_count++] = (held_note_t){
        .pcm = pcm ? pcm : s_rec,
        .frames = s_rec_n,
        .at_us = esp_timer_get_time(),
        .tries = tried,
    };
    s_rec = NULL;
    if (tried) {
        back_off();
    }
    ESP_LOGI(TAG, "saved a %.1fs note to send later (%d waiting)", (double)s_rec_n / MUSE_AUDIO_RATE, s_held_count);
    go_idle(tried ? "SAVED, WILL TRY AGAIN" : "SAVED, SENDS WHEN ONLINE");
}

static void drop_oldest(void)
{
    free(s_held[0].pcm);
    s_held_count--;
    memmove(s_held, s_held + 1, s_held_count * sizeof(s_held[0]));
    if (!s_held_count) {
        s_next_send_us = 0;
        s_send_backoff_us = RETRY_MIN_US;
    }
}

typedef enum {
    FED,
    FEED_FAILED,    /* the turn failed, or Hatch stopped taking audio */
    FEED_PRESSED,   /* a press is queued: it goes first */
    FEED_CANCELLED,
} feed_t;

/*
 * Gives Hatch a note from *sent on, waiting for room as it goes, then ends
 * the turn. Stops for a press only if `yield`.
 */
static feed_t feed_rest(const int16_t *pcm, size_t frames, size_t *sent, bool yield)
{
    char text[96];
    int64_t moved = esp_timer_get_time();
    for (;;) {
        if (atomic_load(&s_cancel_response)) return FEED_CANCELLED;
        muse_hatch_ev_t ev;
        while ((ev = muse_hatch_turn_event(text, sizeof(text))) != MUSE_HATCH_EV_NONE) {
            if (ev == MUSE_HATCH_EV_ERROR) {
                ESP_LOGW(TAG, "muse: %s", text);
                return FEED_FAILED;
            }
        }
        if (*sent == frames) {
            break;
        }
        if (yield && press_waiting()) {
            return FEED_PRESSED;
        }
        size_t n = muse_hatch_turn_audio_wait(pcm + *sent, frames - *sent, 100);
        *sent += n;
        int64_t now = esp_timer_get_time();
        if (n) {
            moved = now;
        } else if (now - moved > STALL_US) {
            ESP_LOGW(TAG, "Muse stopped taking the note");
            return FEED_FAILED;
        }
    }
    muse_hatch_turn_end();
    return FED;
}

/* Asleep: waits for the VM to have the note just sent. */
static bool wait_delivered(void)
{
    char text[96];
    int64_t give_up = esp_timer_get_time() + ACK_WAIT_US;
    while (esp_timer_get_time() < give_up && !atomic_load(&s_power_off_requested)) {
        muse_hatch_ev_t ev;
        while ((ev = muse_hatch_turn_event(text, sizeof(text))) != MUSE_HATCH_EV_NONE) {
            if (ev == MUSE_HATCH_EV_SENT || ev == MUSE_HATCH_EV_REPLY || ev == MUSE_HATCH_EV_DONE) {
                return true;
            }
            if (ev == MUSE_HATCH_EV_ERROR) {
                ESP_LOGW(TAG, "muse: %s", text);
                return false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGW(TAG, "no word that the note arrived");
    return false;
}

/*
 * Sends the oldest saved note. Awake, it's a turn like any other, reply and
 * all; asleep (quiet), it's done once the VM has the note, and the reply
 * waits in the app. A press while it goes up comes first, and the note stays
 * saved. Returns true if a press interrupted the reply.
 */
static bool send_held(bool quiet)
{
    held_note_t *h = &s_held[0];
    ESP_LOGI(TAG, "sending a saved note%s: %.1fs, from %llds ago, try %d", quiet ? " (asleep)" : "",
             (double)h->frames / MUSE_AUDIO_RATE, (long long)((esp_timer_get_time() - h->at_us) / 1000000),
             h->tries + 1);
    if (!quiet) {
        muse_state_set_mode(MUSE_MODE_THINKING);
        muse_state_set_caption("SENDING SAVED NOTE");
    }
    muse_hatch_turn_begin();
    size_t sent = 0;
    feed_t fed = feed_rest(h->pcm, h->frames, &sent, true);
    if (fed == FEED_CANCELLED) {
        atomic_store(&s_cancel_response, false);
        muse_hatch_turn_cancel();
        drop_oldest();
        if (!quiet) go_idle("STOPPED");
        return false;
    }
    bool delivered = false, interrupted = false;
    if (fed == FED && quiet) {
        delivered = wait_delivered();
    } else if (fed == FED) {
        interrupted = hatch_reply(&delivered);
    }
    if (fed != FED || quiet) {
        muse_hatch_turn_cancel();   /* asleep, the reply is left for the app */
    }
    if (fed == FEED_PRESSED) {
        if (!quiet) {
            go_idle("");
        }
        return false;
    }
    const char *caption;
    if (delivered || interrupted) {
        drop_oldest();
        s_next_send_us = 0;   /* the next one right away */
        s_send_backoff_us = RETRY_MIN_US;
        if (quiet) {
            muse_state_set_caption("SAVED NOTE SENT");
        }
        return interrupted;
    } else if (++h->tries >= HELD_TRIES) {
        ESP_LOGW(TAG, "giving up on a saved note after %d tries", h->tries);
        drop_oldest();
        caption = "COULDN'T SEND A SAVED NOTE";
    } else {
        back_off();
        caption = "SAVED NOTE: WILL TRY AGAIN";
    }
    if (quiet) {
        muse_state_set_caption("%s", caption);
    } else {
        go_idle(caption);
    }
    return false;
}

/*
 * Whether to send a saved note now: Hatch within reach and the backoff over.
 * Coming within reach starts the backoff over. Keeps s_waiting too.
 */
static bool held_due(void)
{
    static bool was_ready = true;
    if (!s_held_count) {
        was_ready = true;
        s_waiting = false;
        return false;
    }
    int64_t now = esp_timer_get_time();
    s_waiting = now - s_held[0].at_us < HELD_KEEP_US;
    bool ready = muse_hatch_ready();
    if (ready && !was_ready) {
        s_next_send_us = 0;
        s_send_backoff_us = RETRY_MIN_US;
    }
    was_ready = ready;
    return ready && now >= s_next_send_us;
}
#endif

/*
 * After the release: the rest of the note goes to Hatch and the reply plays,
 * or if it can't go now it's saved to send later. Returns true if a press
 * interrupted the reply.
 */
static bool finish_note(void)
{
    bool delivered;
#if HOLD_NOTES
    if (s_rec) {
        bool fed = false, interrupted = false;
        delivered = false;
        if (s_live && s_sent == s_rec_n) {
            muse_hatch_turn_end();   /* before the chirp, which takes ~90 ms */
            fed = true;
        }
        muse_audio_chirp(0);
        if (s_live && !fed) {
            /* Hatch is behind (still connecting, say): the rest from the kept note. */
            muse_state_set_mode(MUSE_MODE_THINKING);
            muse_state_set_caption("SENDING VOICE NOTE");
            feed_t result = feed_rest(s_rec, s_rec_n, &s_sent, false);
            if (result == FEED_CANCELLED) {
                atomic_store(&s_cancel_response, false);
                muse_hatch_turn_cancel();
                drop_rec();
                go_idle("STOPPED");
                return false;
            }
            fed = result == FED;
            if (!fed) {
                muse_hatch_turn_cancel();
            }
        }
        if (fed) {
            interrupted = hatch_reply(&delivered);
        }
        if (delivered || interrupted) {
            drop_rec();
        } else {
            hold_rec(s_tried);
        }
        return interrupted;
    }
#endif
    muse_hatch_turn_end();   /* before the chirp, which takes ~90 ms */
    muse_audio_chirp(0);
    return hatch_reply(&delivered);
}

/*
 * Whether a press can record: Hatch within reach, or (HOLD_NOTES) set up
 * with room to save the note. If not, the caption says why.
 */
static bool can_record(void)
{
    if (!muse_wifi_connected()) {
        muse_wifi_apply();   /* retry now, not after the backoff */
    }
    bool ready = muse_hatch_ready();
#if HOLD_NOTES
    muse_hatch_status_t st;
    muse_hatch_status(&st);
    if (st.state == MUSE_HATCH_NOT_SET) {
        go_idle("SET UP MUSE FIRST");
        return false;
    }
    if ((!ready || s_held_count) && s_held_count >= HELD_MAX) {
        go_idle("NOTES STILL WAITING TO SEND");
        return false;
    }
    if (ready && s_held_count) {
        s_next_send_us = 0;   /* the saved ones go first, right after */
    }
    s_rec = heap_caps_malloc(MAX_FRAMES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rec) {
        ESP_LOGE(TAG, "no memory to keep a note");
    }
#endif
    if (!ready && !s_rec) {
        go_idle(not_ready_reason());
        return false;
    }
    return true;
}

/* Reuse the local PCM, without asking Hatch to make another turn. */
static bool replay_last_audio(void)
{
    if (!s_last_pcm || !s_last_frames || !muse_settings_speaker_on()) return false;
    atomic_store(&s_replaying, true);
    atomic_store(&s_pause_response, false);
    atomic_store(&s_cancel_response, false);
    muse_state_set_mode(MUSE_MODE_SPEAKING);
    muse_state_set_caption("REPLAYING LAST ANSWER");
    bool pressed = false;
    bool pause_caption = false;
    for (size_t i = 0; i < s_last_frames;) {
        if (atomic_exchange(&s_cancel_response, false)) break;
        if (got_event(MUSE_PTT_DOWN)) {
            pressed = true;
            break;
        }
        if (atomic_load(&s_pause_response)) {
            muse_state_set_level(0);
            if (!pause_caption) {
                muse_state_set_caption("PAUSED - TAP MUSE TO RESUME");
                pause_caption = true;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (pause_caption) {
            pause_caption = false;
            muse_state_set_caption("REPLAYING LAST ANSWER");
        }
        size_t n = s_last_frames - i;
        if (n > MUSE_AUDIO_CHUNK) n = MUSE_AUDIO_CHUNK;
        muse_state_set_level(muse_audio_level(s_last_pcm + i, n));
        muse_audio_write(s_last_pcm + i, n);
        i += n;
    }
    muse_state_set_level(0);
    pre_reset();
    go_idle("");
    atomic_store(&s_replaying, false);
    atomic_store(&s_pause_response, false);
    return pressed;
}

/* Own the output on the voice task: replies and the shutdown cue must not
 * write the shared I2S buffer concurrently. Keep codecs quiet until power cuts. */
static void power_off_audio(void)
{
    muse_hatch_turn_cancel();
    muse_audio_power(true);
    if (muse_settings_speaker_on()) muse_audio_shutdown();
    muse_audio_power(false);
    muse_state_set_mode(MUSE_MODE_OFF);
    muse_state_set_caption("GOODBYE!");
}

static void voice_task(void *arg)
{
    bool pending_down = false;
    bool power_off_played = false;
    muse_audio_selftest();
    if (muse_settings_startup_sound() && muse_settings_speaker_on() && !muse_state_asleep()) {
        muse_audio_startup();
        pre_reset();
    }
    for (;;) {
        if (atomic_load(&s_power_off_requested)) {
            pending_down = false;
            if (!power_off_played) {
                power_off_audio();
                power_off_played = true;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (power_off_played) muse_audio_power(true);   /* shutdown failed: resume voice */
        power_off_played = false;
        bool wake = false;
        if (!pending_down) {
            muse_input_event_t ev;
            bool asleep = muse_state_asleep();
            bool battery = muse_state_on_battery();
            bool rest = asleep && battery && !s_chirp && !s_mp3test && !s_loopback;
#if HOLD_NOTES
            /* A press goes first: send_held() leaves it queued and returns
             * without backing off, so retrying before it's read would spin. */
            if (held_due() && !press_waiting()) {
                atomic_store(&s_cancel_response, false);
                set_resting(false);   /* full power while it goes, even asleep */
                muse_wifi_power(MUSE_WIFI_FULL);
                pending_down = send_held(asleep);
                pre_reset();
                if (!pending_down && !asleep && muse_state_mode(NULL) != MUSE_MODE_IDLE) {
                    muse_state_make_happy();
                    go_idle("");
                }
                continue;
            }
#endif
            set_resting(rest);
            if (!rest && atomic_exchange(&s_replay_request, false)) {
                pending_down = replay_last_audio();
                continue;
            }
            /* On battery Wi-Fi rests with the codecs, and between turns dozes
             * between beacons as xiaozhi's idle does; a press wakes it fully. */
            muse_wifi_power(rest ? MUSE_WIFI_REST : battery ? MUSE_WIFI_DOZE : MUSE_WIFI_FULL);
            if (rest) {
                /* Wait for waking, USB power or a bench request without
                 * polling (the timeout is only a backstop, or a look for
                 * Wi-Fi to send saved notes), so the CPU can stay asleep. */
                muse_state_wait_awake(s_waiting ? HELD_POLL_MS : REST_BACKSTOP_MS);
                continue;
            }
            if (s_chirp) {
                s_chirp = false;
                muse_audio_chirp(1);
                pre_reset();
            }
            if (s_mp3test) {
                s_mp3test = false;
                int16_t *pcm = NULL;
                size_t n = muse_hatch_mp3_selftest(&pcm);
                for (size_t i = 0; i < n; i += MUSE_AUDIO_CHUNK) {
                    muse_audio_write(pcm + i, n - i < MUSE_AUDIO_CHUNK ? n - i : MUSE_AUDIO_CHUNK);
                }
                free(pcm);
                pre_reset();
            }
            if (s_loopback) {
                s_loopback = false;
                muse_audio_loopback_test(muse_settings_volume());
                pre_reset();
            }
            /* The 20 ms read paces this loop. */
            idle_capture();
            if (xQueueReceive(s_queue, &ev, 0) != pdTRUE) {
                continue;
            }
            muse_state_poke();
            if (ev.type != MUSE_PTT_DOWN) {
                continue;
            }
            wake = ev.wake;
        }
        if (wake && !held_on_waking()) {
            continue;   /* a tap: it only woke Muse */
        }
        if (atomic_load(&s_power_off_requested)) continue;
        muse_wifi_power(MUSE_WIFI_FULL);
        atomic_store(&s_cancel_response, false);
        if (!can_record()) {
            pending_down = false;
            continue;
        }
        size_t held;
        char why[96];
        /* The rising cue marks an accepted talk press. Drain its acoustic tail
         * before capture so the note starts with the user's voice. */
        muse_state_set_mode(MUSE_MODE_LISTENING);
        muse_state_set_caption("LISTENING...");
        muse_audio_chirp(1);
        for (int i = 0; i < SETTLE_CHUNKS; i++) {
            muse_audio_read(s_chunk, MUSE_AUDIO_CHUNK);
        }
        pre_reset();
        bool ok = record(&held, why, sizeof(why));
        pending_down = false;
        if (atomic_load(&s_power_off_requested)) {
            muse_hatch_turn_cancel();
            drop_rec();
            continue;
        }
        if (held < MIN_HELD_FRAMES && !wake) {
            muse_hatch_turn_cancel();
            drop_rec();
            pre_reset();
            go_idle("HOLD LONGER TO TALK");
            continue;
        }
        if (!ok) {
            pre_reset();
            go_idle(why);   /* Hatch failed it while recording */
            continue;
        }
        pending_down = finish_note();
        pre_reset();
        if (!pending_down && muse_state_mode(NULL) != MUSE_MODE_IDLE) {
            muse_state_make_happy();
            go_idle("");
        }
    }
}

esp_err_t muse_voice_start(QueueHandle_t queue)
{
    s_queue = queue;
    s_last_lock = xSemaphoreCreateMutex();
    s_pre = heap_caps_malloc(PRE_CHUNKS * sizeof(pre_chunk_t), MUSE_BIG_CAPS);
    if (!s_last_lock || !s_pre || muse_audio_init(muse_settings_volume(), muse_settings_mic_gain()) != ESP_OK) {
        muse_state_set_mode(MUSE_MODE_ERROR);
        muse_state_set_caption("AUDIO INIT FAILED");
        return ESP_FAIL;
    }
    /* Stack in PSRAM if there is any (this task never writes flash) to spare internal RAM for Wi-Fi/BLE. */
    if (xTaskCreatePinnedToCoreWithCaps(voice_task, "muse_voice", 6144, NULL, 6, NULL, MUSE_AUDIO_CORE,
                                        MUSE_BIG_CAPS) != pdPASS) {
        /* Without the task nothing would ever rest the codecs, which
         * muse_audio_init() left open: on the Watcher that's ~33 mA. */
        muse_audio_power(false);
        muse_state_set_mode(MUSE_MODE_ERROR);
        muse_state_set_caption("AUDIO INIT FAILED");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void muse_voice_set_monitor(bool on)
{
    s_monitor_db = -100.0f;
    s_monitor = on;
}

float muse_voice_monitor_db(void)
{
    return s_monitor_db;
}

void muse_voice_power_off(bool shutting_down)
{
    atomic_store(&s_power_off_requested, shutting_down);
    if (shutting_down) {
        muse_voice_cancel_response();
        muse_state_nudge();
    }
}

void muse_voice_request_chirp(void)
{
    s_chirp = true;
    muse_state_nudge();
}

void muse_voice_request_loopback(void)
{
    s_loopback = true;
    muse_state_nudge();
}

void muse_voice_request_mp3test(void)
{
    s_mp3test = true;
    muse_state_nudge();
}

bool muse_voice_resting(void)
{
    return s_resting;
}

bool muse_voice_notes_waiting(void)
{
    return s_waiting;
}
