/* Copyright (c) Meta Platforms, Inc. and affiliates.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "muse_model.h"
#include <stdlib.h>
#include <string.h>

char *muse_model_body(muse_provider_t p, const char *model, const char *text)
{
    if (p <= MUSE_PROVIDER_MUSE || p >= MUSE_PROVIDER_COUNT || !model || !text) return NULL;
    const char *template = p == MUSE_PROVIDER_GEMINI ?
        "{\"contents\":[{\"role\":\"user\",\"parts\":[{\"text\":\"\"}]}],\"generationConfig\":{\"maxOutputTokens\":2048}}" :
        p == MUSE_PROVIDER_OPENAI ?
        "{\"model\":\"\",\"messages\":[{\"role\":\"user\",\"content\":\"\"}],\"max_completion_tokens\":2048}" :
        "{\"model\":\"\",\"messages\":[{\"role\":\"user\",\"content\":\"\"}],\"max_tokens\":2048}";
    cJSON *root = cJSON_Parse(template);
    if (!root) return NULL;
    cJSON *field;
    if (p == MUSE_PROVIDER_GEMINI) {
        cJSON *message = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "contents"), 0);
        cJSON *part = cJSON_GetArrayItem(cJSON_GetObjectItem(message, "parts"), 0);
        field = cJSON_GetObjectItem(part, "text");
    } else {
        cJSON *message = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "messages"), 0);
        field = cJSON_GetObjectItem(message, "content");
        if (!cJSON_SetValuestring(cJSON_GetObjectItem(root, "model"), model)) {
            cJSON_Delete(root);
            return NULL;
        }
    }
    if (!cJSON_SetValuestring(field, text)) { cJSON_Delete(root); return NULL; }
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

/* Join text blocks only. Reasoning/tool blocks must never become spoken text. */
char *muse_model_reply(muse_provider_t p, const cJSON *root)
{
    const cJSON *parts = NULL;
    if (p == MUSE_PROVIDER_OPENAI) {
        const cJSON *choice = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "choices"), 0);
        const cJSON *message = cJSON_GetObjectItem(choice, "message");
        const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(message, "content"));
        return text && text[0] && strlen(text) <= 16384 ? strdup(text) : NULL;
    }
    if (p == MUSE_PROVIDER_GEMINI) {
        const cJSON *candidate = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "candidates"), 0);
        parts = cJSON_GetObjectItem(cJSON_GetObjectItem(candidate, "content"), "parts");
    } else if (p == MUSE_PROVIDER_ANTHROPIC) {
        parts = cJSON_GetObjectItem(root, "content");
    } else return NULL;
    char *out = calloc(1, 16385);
    if (!out) return NULL;
    size_t have = 0;
    const cJSON *part;
    cJSON_ArrayForEach(part, parts) {
        const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(part, "type"));
        if (p == MUSE_PROVIDER_ANTHROPIC && (!type || strcmp(type, "text"))) continue;
        if (cJSON_IsTrue(cJSON_GetObjectItem(part, "thought"))) continue;
        const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(part, "text"));
        if (!text) continue;
        size_t n = strlen(text);
        if (n > 16384 - have) { free(out); return NULL; }
        memcpy(out + have, text, n + 1);
        have += n;
    }
    if (!have) { free(out); return NULL; }
    return out;
}
