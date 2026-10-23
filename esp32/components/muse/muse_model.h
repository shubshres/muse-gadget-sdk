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
#define MUSE_MODEL_HISTORY_PAIRS 8
#define MUSE_MODEL_HISTORY_BYTES (32 * 1024)
typedef struct {
    cJSON *messages;             /* canonical user/assistant text messages */
    size_t bytes;
    unsigned pairs;
} muse_model_history_t;
void muse_model_history_clear(muse_model_history_t *history);
bool muse_model_history_append(muse_model_history_t *history, const char *prompt, const char *reply);
char *muse_model_body_with_history(muse_provider_t provider, const char *model,
                                  const char *text, const cJSON *history);
char *muse_model_reply(muse_provider_t provider, const cJSON *json);

typedef struct {
    uint32_t request;
    uint32_t conversation;
    char *prompt;                /* caller frees; input transcript or typed text */
    char *text;                  /* caller frees */
    char *heard;                 /* caller frees */
    char error[64];              /* never includes response bodies or credentials */
} muse_model_result_t;
bool muse_model_init(void);
/* Copies input and credentials. PCM is mono 16-bit, 16 kHz. Only one job at a time. */
bool muse_model_start(uint32_t request, muse_provider_t provider,
                      const char *text, const uint8_t *pcm, size_t bytes);
/* wav has 44 bytes of header space followed by `bytes` of PCM. Takes ownership
 * only on success; on failure the caller retains and frees the buffer. */
bool muse_model_start_voice(uint32_t request, muse_provider_t provider, uint8_t *wav, size_t bytes);
bool muse_model_poll(muse_model_result_t *out);
/* Session task only: commit only replies accepted for the current turn. */
void muse_model_accept(const muse_model_result_t *result);
void muse_model_forget(void);
#ifdef __cplusplus
}
#endif
