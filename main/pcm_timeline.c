#include "pcm_timeline.h"

void pcm_timeline_reset(pcm_timeline_t *t)
{
    t->count = 0;
    t->end = 0;
}

void pcm_timeline_write(pcm_timeline_t *t, int64_t start, const int16_t *samples, size_t count)
{
    /* Absorb at most one sample of DMA interrupt timestamp jitter. A real
     * discontinuity starts a new history rather than joining unrelated audio. */
    if (t->count && start >= t->end - 1 && start <= t->end + 1) {
        start = t->end;
    } else if (start != t->end) {
        t->count = 0;
    }
    for (size_t i = 0; i < count; ++i) {
        int64_t tick = start + (int64_t)i;
        if (tick >= 0) {
            t->samples[(uint64_t)tick % t->capacity] = samples[i];
        }
    }
    t->end = start + (int64_t)count;
    t->count = t->count + count > t->capacity ? t->capacity : t->count + count;
}

size_t pcm_timeline_read(const pcm_timeline_t *t, int64_t start, int16_t *samples, size_t count)
{
    size_t available = 0;
    for (size_t i = 0; i < count; ++i) {
        int64_t tick = start + (int64_t)i;
        bool valid = tick >= 0 && tick >= t->end - (int64_t)t->count && tick < t->end;
        samples[i] = valid ? t->samples[(uint64_t)tick % t->capacity] : 0;
        available += valid;
    }
    return available;
}

void pcm_mix_s16(int16_t *out, const int16_t *first, const int16_t *second, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        int32_t value = (int32_t)first[i] + second[i];
        out[i] = value > INT16_MAX ? INT16_MAX : value < INT16_MIN ? INT16_MIN : (int16_t)value;
    }
}
