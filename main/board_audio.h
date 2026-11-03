#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define BOARD_AUDIO_RATE 48000
#define BOARD_AUDIO_SOURCES 2

esp_err_t board_audio_init(unsigned volume);
bool board_audio_ready(void);
void board_audio_set_volume(unsigned volume);
void board_audio_set_route(int source);
void board_audio_set_output_enabled(unsigned source, bool enabled);
bool board_audio_write(unsigned source, const int16_t *samples, size_t count);
size_t board_audio_read(unsigned source, int16_t *samples, size_t count, uint32_t timeout_ms);
unsigned board_audio_input_level(void);
bool board_audio_is_speaking(void);
