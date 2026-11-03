#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef enum {
    SESSION_MODE_LIVE_MEETING,
    SESSION_MODE_PORTABLE,
    SESSION_MODE_MEDIA,
    SESSION_MODE_COUNT,
} session_mode_t;

typedef enum {
    SESSION_TRAVEL_NONE,
    SESSION_TRAVEL_SOURCE_LANGUAGE,
    SESSION_TRAVEL_TARGET_LANGUAGE,
} session_travel_turn_t;

esp_err_t session_init(void);
void session_request(bool start);
bool session_requested(void);
void session_set_languages(size_t source_index, size_t target_index);
void session_set_voice(const char *voice_id);
void session_set_mode(session_mode_t mode);
session_mode_t session_get_mode(void);
void session_set_travel_turn(session_travel_turn_t turn);
session_travel_turn_t session_get_travel_turn(void);

