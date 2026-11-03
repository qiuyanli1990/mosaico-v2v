#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_webrtc.h"

#define MEDIA_DEFAULT_SPEAKER_VOLUME 70U

typedef enum {
    MEDIA_SOURCE_USB_SPEAKER,
    MEDIA_SOURCE_ONBOARD_MIC,
    MEDIA_SOURCE_COUNT,
} media_source_t;

esp_err_t media_init(void);
esp_err_t media_session_open(media_source_t source, esp_webrtc_media_provider_t *provider);
void media_session_close(media_source_t source);
void media_set_enabled(media_source_t source, bool enabled);
void media_set_portable_mode(bool portable);
void media_set_portable_input(media_source_t source);
unsigned media_get_portable_input_level(void);
bool media_is_portable_mode(void);
void media_set_volume(unsigned volume);
unsigned media_get_volume(void);
uint32_t media_rendered_frames(void);
bool media_is_speaking(void);

