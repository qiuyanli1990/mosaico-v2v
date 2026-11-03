#include "pcm_ring.h"

void pcm_ring_reset(pcm_ring_t *ring)
{
    ring->head = 0;
    ring->count = 0;
}

void pcm_ring_write(pcm_ring_t *ring, const int16_t *samples, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (ring->count == ring->capacity) {
            ring->head = (ring->head + 1) % ring->capacity;
            --ring->count;
            ++ring->dropped;
        }
        ring->samples[(ring->head + ring->count) % ring->capacity] = samples[i];
        ++ring->count;
    }
}

size_t pcm_ring_read(pcm_ring_t *ring, int16_t *samples, size_t count)
{
    size_t available = ring->count < count ? ring->count : count;
    for (size_t i = 0; i < available; ++i) {
        samples[i] = ring->samples[(ring->head + i) % ring->capacity];
    }
    ring->head = (ring->head + available) % ring->capacity;
    ring->count -= available;
    return available;
}
