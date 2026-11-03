#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* A bounded PCM history addressed by monotonic sample ticks, not FIFO order.
 * The caller serializes access. Unavailable/expired samples read as zero. */
typedef struct {
    int16_t *samples;
    size_t capacity;
    size_t count;
    int64_t end;
} pcm_timeline_t;

void pcm_timeline_reset(pcm_timeline_t *timeline);
void pcm_timeline_write(pcm_timeline_t *timeline, int64_t start,
                        const int16_t *samples, size_t count);
size_t pcm_timeline_read(const pcm_timeline_t *timeline, int64_t start,
                         int16_t *samples, size_t count);
void pcm_mix_s16(int16_t *out, const int16_t *first, const int16_t *second, size_t count);
