#include "pcm_ring.h"
#include <stdbool.h>
#include <string.h>

static bool overlaps_storage(const pcm_ring_t *ring, const int16_t *samples, size_t count)
{
    uintptr_t storage = (uintptr_t)ring->samples;
    uintptr_t data = (uintptr_t)samples;
    return data >= storage ? data - storage < ring->capacity * sizeof(*samples) :
                             storage - data < count * sizeof(*samples);
}

void pcm_ring_reset(pcm_ring_t *ring)
{
    ring->head = 0;
    ring->count = 0;
}

void pcm_ring_write(pcm_ring_t *ring, const int16_t *samples, size_t count)
{
    if (!count) return;
    /* Preserve in-place callers; ordinary capture/render buffers use bulk copy. */
    if (overlaps_storage(ring, samples, count)) {
        for (size_t i = 0; i < count; ++i) {
            if (ring->count == ring->capacity) {
                ring->head = (ring->head + 1) % ring->capacity;
                --ring->count;
                ++ring->dropped;
            }
            ring->samples[(ring->head + ring->count) % ring->capacity] = samples[i];
            ++ring->count;
        }
        return;
    }
    size_t space = ring->capacity - ring->count;
    if (count > space) {
        size_t dropped = count - space;
        ring->head = (ring->head + dropped % ring->capacity) % ring->capacity;
        ring->count -= dropped < ring->count ? dropped : ring->count;
        ring->dropped += dropped;
    }
    /* An oversized burst retains the same tail without copying discarded PCM. */
    if (count > ring->capacity) {
        samples += count - ring->capacity;
        count = ring->capacity;
    }
    size_t tail = (ring->head + ring->count) % ring->capacity;
    size_t first = ring->capacity - tail;
    if (first > count) first = count;
    memcpy(ring->samples + tail, samples, first * sizeof(*samples));
    if (count > first) {
        memcpy(ring->samples, samples + first, (count - first) * sizeof(*samples));
    }
    ring->count += count;
}

size_t pcm_ring_read(pcm_ring_t *ring, int16_t *samples, size_t count)
{
    size_t available = ring->count < count ? ring->count : count;
    if (!available) return 0;
    if (overlaps_storage(ring, samples, available)) {
        for (size_t i = 0; i < available; ++i) {
            samples[i] = ring->samples[(ring->head + i) % ring->capacity];
        }
        ring->head = (ring->head + available) % ring->capacity;
        ring->count -= available;
        return available;
    }
    size_t first = ring->capacity - ring->head;
    if (first > available) first = available;
    memcpy(samples, ring->samples + ring->head, first * sizeof(*samples));
    if (available > first) {
        memcpy(samples + first, ring->samples, (available - first) * sizeof(*samples));
    }
    ring->head = (ring->head + available) % ring->capacity;
    ring->count -= available;
    return available;
}
