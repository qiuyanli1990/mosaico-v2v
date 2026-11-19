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
    uint64_t mic_written_samples;
    uint64_t mic_underrun_samples;
    uint32_t mic_underruns;
    uint32_t mic_write_waits;
    uint32_t mic_callback_late;
    uint32_t mic_callback_max_gap_us;
    uint32_t mic_render_frames;
    uint32_t mic_pts_same;
    uint32_t mic_pts_back;
    uint32_t mic_repeat_hints;
    size_t mic_buffered_samples;
    bool mic_active;
} usb_audio_stats_t;

esp_err_t usb_audio_init(void);
bool usb_audio_is_mounted(void);
void usb_audio_set_forwarding(bool speaker_input, bool mic_output_enabled);
void usb_audio_clear(void);
size_t usb_audio_read(int16_t *samples, size_t count);
void usb_audio_write_microphone(const int16_t *samples, size_t count);
/* USB ISR only, after init. Pads silence while inactive/prefilling or on shortage. */
size_t usb_audio_read_microphone_isr(int16_t *samples, size_t count);
void usb_audio_trace_microphone_frame(uint32_t pts, const uint8_t *data, size_t bytes);
void usb_audio_set_microphone_active(bool active);
void usb_audio_get_stats(usb_audio_stats_t *stats);
void usb_audio_log_diagnostics(void);

