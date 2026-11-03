#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    bool mounted;
    bool streaming;
    unsigned level;
    unsigned host_volume;
    bool host_muted;
    uint64_t bytes_received;
    uint64_t dropped_samples;
    size_t buffered_samples;
    uint64_t mic_dropped_samples;
} usb_audio_stats_t;

esp_err_t usb_audio_init(void);
bool usb_audio_is_mounted(void);
void usb_audio_set_forwarding(bool speaker_input, bool mic_output_enabled);
void usb_audio_clear(void);
size_t usb_audio_read(int16_t *samples, size_t count);
void usb_audio_write_microphone(const int16_t *samples, size_t count);
void usb_audio_get_stats(usb_audio_stats_t *stats);
void usb_audio_log_diagnostics(void);

