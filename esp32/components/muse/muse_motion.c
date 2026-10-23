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

#include "muse_motion.h"

/* The 2 g range yields 16384 counts per g. Reject impacts and free fall. */
#define ONE_G_SQUARED (16384LL * 16384LL)

static bool steady_gravity(muse_motion_sample_t v)
{
    int64_t length = (int64_t)v.x * v.x + (int64_t)v.y * v.y + (int64_t)v.z * v.z;
    return length > ONE_G_SQUARED * 65 / 100 && length < ONE_G_SQUARED * 150 / 100;
}

static int64_t dot(muse_motion_sample_t a, muse_motion_sample_t b)
{
    return (int64_t)a.x * b.x + (int64_t)a.y * b.y + (int64_t)a.z * b.z;
}

unsigned muse_motion_update(muse_motion_gesture_t *g, muse_motion_sample_t v,
                            bool asleep, bool replying, bool wake_enabled, bool silence_enabled,
                            uint32_t now_ms)
{
    unsigned events = 0;
    if (!steady_gravity(v)) {
        g->wake_since_ms = g->flip_since_ms = 0;
        return 0;
    }

    if (!g->initialized || asleep != g->was_asleep) {
        g->initialized = true;
        g->flip_pose = v;
        g->flip_armed = !asleep && silence_enabled;
        g->flip_since_ms = 0;
        g->was_asleep = asleep;
        g->sleep_pose = v;
        g->wake_armed = asleep && wake_enabled;
        g->wake_since_ms = 0;
    }
    if (wake_enabled != g->was_wake_enabled) {
        g->was_wake_enabled = wake_enabled;
        g->sleep_pose = v;
        g->wake_armed = asleep && wake_enabled;
        g->wake_since_ms = 0;
    }
    if (silence_enabled != g->was_silence_enabled) {
        g->was_silence_enabled = silence_enabled;
        g->flip_pose = v;
        g->flip_armed = !asleep && silence_enabled;
        g->flip_since_ms = 0;
    }

    /* A deliberate ~50-degree change from the pose at sleep, held 250 ms. */
    if (asleep && wake_enabled && g->wake_armed && dot(g->sleep_pose, v) < ONE_G_SQUARED * 64 / 100) {
        if (!g->wake_since_ms) g->wake_since_ms = now_ms ? now_ms : 1;
        if (now_ms - g->wake_since_ms >= 250) {
            g->wake_armed = false;
            events |= MUSE_MOTION_WAKE;
        }
    } else {
        g->wake_since_ms = 0;
    }

    /* Turn over from the awake pose: >120 degrees, held 450 ms.
     * Capture a new pose after waking so a stationary board cannot re-trigger. */
    if (!asleep && silence_enabled && g->flip_armed && dot(g->flip_pose, v) < -ONE_G_SQUARED / 2) {
        if (!g->flip_since_ms) g->flip_since_ms = now_ms ? now_ms : 1;
        if (now_ms - g->flip_since_ms >= 450) {
            g->flip_armed = false;
            events |= MUSE_MOTION_SLEEP;
            if (replying) events |= MUSE_MOTION_SILENCE;
        }
    } else {
        g->flip_since_ms = 0;
    }
    return events;
}
