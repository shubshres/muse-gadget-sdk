/* Copyright (c) Meta Platforms, Inc. and affiliates.
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "muse_settings.h"
#include "cJSON.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Bounded request/response codecs, also exercised by the host tests. */
char *muse_model_body(muse_provider_t provider, const char *model, const char *text);
char *muse_model_reply(muse_provider_t provider, const cJSON *json);

typedef struct {
    uint32_t request;
    char *text;                  /* caller frees */
    char *heard;                 /* caller frees */
    char error[64];              /* never includes response bodies or credentials */
} muse_model_result_t;
bool muse_model_init(void);
/* Copies input and credentials. PCM is mono 16-bit, 16 kHz. Only one job at a time. */
bool muse_model_start(uint32_t request, muse_provider_t provider,
                      const char *text, const uint8_t *pcm, size_t bytes);
bool muse_model_poll(muse_model_result_t *out);
#ifdef __cplusplus
}
#endif
