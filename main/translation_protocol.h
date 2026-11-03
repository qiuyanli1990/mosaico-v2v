#pragma once
#include <stdbool.h>
#include <stddef.h>

#define TRANSLATION_LANGUAGE_COUNT 60U
#define TRANSLATION_VOICE_ID_SIZE 64U

typedef struct {
    const char *code;
    const char *english_name;
    const char *native_name;
    bool tts_supported;
} translation_language_t;

typedef struct {
    const char *id;
    const char *name;
    const char *language;
} translation_voice_t;

typedef struct {
    const char *channel;
    const char *device_uid;
    const char *agent_uid;
    const char *voice_id;
    float tts_speed;
    const char *source_language;
    const char *target_language;
    bool tts_enabled;
    unsigned idle_timeout;
} translation_request_t;

const translation_language_t *translation_language_at(size_t index);
size_t translation_language_index(const char *code);
size_t translation_tts_language_count(void);
bool translation_language_tts_supported(const char *code);
size_t translation_voice_count_for_language(const char *language_code);
const translation_voice_t *translation_voice_for_language(const char *language_code,
                                                          size_t option_index);
/* Accept a raw MiniMax ID or an already-prefixed V2V ID. */
bool translation_format_voice_id(const char *id, char *out, size_t capacity);

/* Returned JSON belongs to the caller and must be freed with cJSON_free(). */
char *translation_join_json(const translation_request_t *request);
bool translation_parse_agent(const char *json, char *agent_id, size_t capacity);
bool translation_parse_status(const char *json, char *status, size_t capacity);
bool translation_valid_agent_id(const char *agent_id);
bool translation_channel_from_random(const unsigned char random[16], char *channel, size_t capacity);

