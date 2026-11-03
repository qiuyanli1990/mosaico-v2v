#pragma once
#include <stddef.h>
#include <stdint.h>

/* The caller serializes access. Overflow discards oldest samples to bound latency. */
typedef struct {
    int16_t *samples;
    size_t capacity;
    size_t head;
    size_t count;
    uint64_t dropped;
} pcm_ring_t;

void pcm_ring_reset(pcm_ring_t *ring);
void pcm_ring_write(pcm_ring_t *ring, const int16_t *samples, size_t count);
size_t pcm_ring_read(pcm_ring_t *ring, int16_t *samples, size_t count);

