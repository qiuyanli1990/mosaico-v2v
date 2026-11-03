#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

bool v2v_configured(void);
esp_err_t v2v_join(const char *channel, const char *source_language,
                   const char *device_uid, const char *target_language,
                   const char *voice_id, char *agent_id, size_t capacity);
esp_err_t v2v_leave(const char *agent_id);
esp_err_t v2v_query(const char *agent_id, char *status, size_t capacity);

