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

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MUSE_MOTION_WAKE    (1u << 0)
#define MUSE_MOTION_SILENCE (1u << 1)
#define MUSE_MOTION_SLEEP   (1u << 2)

typedef struct {
    int16_t x, y, z;  /* QMI8658 acceleration, 16384 counts per g */
} muse_motion_sample_t;

typedef struct {
    muse_motion_sample_t sleep_pose, flip_pose;
    uint32_t wake_since_ms, flip_since_ms;
    bool initialized, was_asleep;
    bool was_wake_enabled, was_silence_enabled;
    bool wake_armed, flip_armed;
} muse_motion_gesture_t;

/* Call with each fresh accelerometer sample. Time is monotonic milliseconds. */
unsigned muse_motion_update(muse_motion_gesture_t *gesture, muse_motion_sample_t sample,
                            bool asleep, bool replying, bool wake_enabled, bool silence_enabled,
                            uint32_t now_ms);
