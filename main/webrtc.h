#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "media.h"

esp_err_t translator_webrtc_start(media_source_t source, const char *channel,
                                  const char *device_uid);
void translator_webrtc_stop(media_source_t source);
bool translator_webrtc_connected(media_source_t source);
bool translator_webrtc_failed(media_source_t source);
void translator_webrtc_query(media_source_t source);

