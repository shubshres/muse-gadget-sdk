/* Copyright (c) Meta Platforms, Inc. and affiliates.
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

/* The session and direct-model worker must accept the same captured audio.
 * Other boards retain the existing headroom for their 15-second recorder. */
#if CONFIG_MUSE_BOARD_WAVESHARE_S3_175C
#define MUSE_VOICE_RECORD_MAX_SECONDS 30
#define MUSE_VOICE_INPUT_MAX_SECONDS MUSE_VOICE_RECORD_MAX_SECONDS
#else
#define MUSE_VOICE_RECORD_MAX_SECONDS 15
#define MUSE_VOICE_INPUT_MAX_SECONDS 20
#endif
#define MUSE_VOICE_INPUT_MAX_BYTES (16000 * 2 * MUSE_VOICE_INPUT_MAX_SECONDS)
