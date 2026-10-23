/* Copyright (c) Meta Platforms, Inc. and affiliates.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "muse_model.h"
#include <stdlib.h>
#include <string.h>

/* cJSON uses the session's PSRAM allocator on the device. */
static cJSON *text_message(muse_provider_t p, const char *role, const char *text)
{
    cJSON *message = cJSON_Parse(p == MUSE_PROVIDER_GEMINI ?
        "{\"role\":\"\",\"parts\":[{\"text\":\"\"}]}" : "{\"role\":\"\",\"content\":\"\"}");
    if (!message) return NULL;
    cJSON *field = p == MUSE_PROVIDER_GEMINI ?
        cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(message, "parts"), 0), "text") :
        cJSON_GetObjectItem(message, "content");
    const char *wire_role = p == MUSE_PROVIDER_GEMINI && !strcmp(role, "assistant") ? "model" : role;
    if (!cJSON_SetValuestring(cJSON_GetObjectItem(message, "role"), wire_role) ||
        !cJSON_SetValuestring(field, text)) {
        cJSON_Delete(message);
        return NULL;
    }
    return message;
}

void muse_model_history_clear(muse_model_history_t *h)
{
    cJSON_Delete(h->messages);
    memset(h, 0, sizeof(*h));
}

bool muse_model_history_append(muse_model_history_t *h, const char *prompt, const char *reply)
{
    if (!prompt || !prompt[0] || !reply || !reply[0]) return false;
    size_t bytes = strlen(prompt) + strlen(reply);
    if (bytes > MUSE_MODEL_HISTORY_BYTES) {
        /* Don't leave stale older context after an exchange too large to retain. */
        muse_model_history_clear(h);
        return false;
    }
    cJSON *user = text_message(MUSE_PROVIDER_OPENAI, "user", prompt);
    cJSON *assistant = text_message(MUSE_PROVIDER_OPENAI, "assistant", reply);
    if (!h->messages) h->messages = cJSON_CreateArray();
    if (!user || !assistant || !h->messages) {
        cJSON_Delete(user);
        cJSON_Delete(assistant);
        return false;
    }
    while (h->pairs && (h->pairs >= MUSE_MODEL_HISTORY_PAIRS || h->bytes + bytes > MUSE_MODEL_HISTORY_BYTES)) {
        for (int i = 0; i < 2; i++) {
            cJSON *old = cJSON_GetArrayItem(h->messages, 0);
            h->bytes -= strlen(cJSON_GetStringValue(cJSON_GetObjectItem(old, "content")));
            cJSON_DeleteItemFromArray(h->messages, 0);
        }
        h->pairs--;
    }
    cJSON_AddItemToArray(h->messages, user);
    cJSON_AddItemToArray(h->messages, assistant);
    h->pairs++;
    h->bytes += bytes;
    return true;
}

char *muse_model_body_with_history(muse_provider_t p, const char *model, const char *text, const cJSON *history)
{
    if (p <= MUSE_PROVIDER_MUSE || p >= MUSE_PROVIDER_COUNT || !model || !text) return NULL;
    if (history && (!cJSON_IsArray(history) || cJSON_GetArraySize(history) % 2 ||
                    cJSON_GetArraySize(history) > 2 * MUSE_MODEL_HISTORY_PAIRS)) return NULL;
    const char *template = p == MUSE_PROVIDER_GEMINI ?
        "{\"contents\":[],\"generationConfig\":{\"maxOutputTokens\":2048}}" :
        p == MUSE_PROVIDER_OPENAI ?
        "{\"model\":\"\",\"messages\":[],\"max_completion_tokens\":2048}" :
        "{\"model\":\"\",\"messages\":[],\"max_tokens\":2048}";
    cJSON *root = cJSON_Parse(template);
    if (!root) return NULL;
    if (p != MUSE_PROVIDER_GEMINI && !cJSON_SetValuestring(cJSON_GetObjectItem(root, "model"), model)) {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON *messages = cJSON_GetObjectItem(root, p == MUSE_PROVIDER_GEMINI ? "contents" : "messages");
    const cJSON *old;
    unsigned index = 0;
    cJSON_ArrayForEach(old, history) {
        const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(old, "role"));
        const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(old, "content"));
        if (!role || strcmp(role, index++ % 2 ? "assistant" : "user") || !content) {
            cJSON_Delete(root);
            return NULL;
        }
        cJSON *message = text_message(p, role, content);
        if (!message) { cJSON_Delete(root); return NULL; }
        cJSON_AddItemToArray(messages, message);
    }
    cJSON *current = text_message(p, "user", text);
    if (!current) { cJSON_Delete(root); return NULL; }
    cJSON_AddItemToArray(messages, current);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

char *muse_model_body(muse_provider_t p, const char *model, const char *text)
{
    return muse_model_body_with_history(p, model, text, NULL);
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
