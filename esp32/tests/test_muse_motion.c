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

#include <assert.h>
#include <stdio.h>

#include "muse_motion.h"

#define FACE_UP ((muse_motion_sample_t){ 0, 0, 16384 })
#define TILTED ((muse_motion_sample_t){ 13000, 0, 9900 })
#define FACE_DOWN ((muse_motion_sample_t){ 0, 0, -16384 })
#define IMPACT ((muse_motion_sample_t){ 30000, 30000, 30000 })

static unsigned update(muse_motion_gesture_t *g, muse_motion_sample_t v, bool asleep, bool replying, uint32_t ms)
{
    return muse_motion_update(g, v, asleep, replying, true, true, ms);
}

int main(void)
{
    muse_motion_gesture_t g = { 0 };
    assert(update(&g, FACE_UP, true, false, 100) == 0);
    assert(update(&g, TILTED, true, false, 150) == 0);
    assert(update(&g, IMPACT, true, false, 450) == 0);
    assert(update(&g, TILTED, true, false, 500) == 0);
    assert(update(&g, TILTED, true, false, 700) == 0);
    assert(update(&g, TILTED, true, false, 751) == MUSE_MOTION_WAKE);
    assert(update(&g, TILTED, true, false, 1100) == 0);

    assert(update(&g, FACE_UP, false, true, 1200) == 0);
    assert(update(&g, FACE_DOWN, false, true, 1300) == 0);
    assert(update(&g, FACE_DOWN, false, true, 1749) == 0);
    assert(update(&g, FACE_DOWN, false, true, 1750) == MUSE_MOTION_SILENCE);
    assert(update(&g, FACE_DOWN, false, true, 2000) == 0);
    assert(update(&g, FACE_DOWN, false, false, 2100) == 0);
    /* Starting a new reply while already inverted does not silence it. */
    assert(update(&g, FACE_DOWN, false, true, 2200) == 0);
    assert(update(&g, FACE_DOWN, false, true, 3000) == 0);

    muse_motion_gesture_t disabled = { 0 };
    assert(muse_motion_update(&disabled, FACE_UP, true, false, false, true, 100) == 0);
    assert(muse_motion_update(&disabled, TILTED, true, false, false, true, 500) == 0);
    assert(muse_motion_update(&disabled, TILTED, true, false, true, true, 600) == 0);
    assert(muse_motion_update(&disabled, FACE_UP, true, false, true, true, 700) == 0);
    assert(muse_motion_update(&disabled, FACE_UP, true, false, true, true, 951) == MUSE_MOTION_WAKE);
    puts("muse motion gestures: OK");
    return 0;
}
