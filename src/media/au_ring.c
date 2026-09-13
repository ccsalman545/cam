#define _POSIX_C_SOURCE 200809L

/*
 * au_ring.c
 */
#include "au_ring.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct AuRing {
    uint8_t *backing;           /* slot_count x slot_capacity */
    AuMeta *meta;
    size_t slot_capacity;
    size_t slot_count;
    size_t head;                /* next to read */
    size_t tail;                /* next to write */
    size_t count;
    uint64_t dropped;
    uint64_t sequence;
    pthread_mutex_t lock;
};

AuRing *au_ring_create(size_t slot_capacity, size_t slot_count)
{
    if (slot_capacity == 0 || slot_count == 0) {
        return NULL;
    }

    AuRing *ring = calloc(1, sizeof(*ring));
    if (ring == NULL) {
        return NULL;
    }

    ring->backing = malloc(slot_capacity * slot_count);
    ring->meta = calloc(slot_count, sizeof(AuMeta));

    if (ring->backing == NULL || ring->meta == NULL) {
        free(ring->backing);
        free(ring->meta);
        free(ring);
        return NULL;
    }

    ring->slot_capacity = slot_capacity;
    ring->slot_count = slot_count;
    pthread_mutex_init(&ring->lock, NULL);

    return ring;
}

int au_ring_push(AuRing *ring,
                 const uint8_t *data,
                 size_t size,
                 uint64_t pts_us,
                 int is_idr)
{
    if (ring == NULL || size == 0 || size > ring->slot_capacity) {
        return -1;
    }

    if (data == NULL) {
        return -1;
    }

    pthread_mutex_lock(&ring->lock);

    if (ring->count == ring->slot_count) {
        /*
         * Overwrite the oldest access unit.
         */
        ring->head = (ring->head + 1) % ring->slot_count;
        ring->count--;
        ring->dropped++;
    }

    size_t tail = ring->tail;

    memcpy(ring->backing + tail * ring->slot_capacity, data, size);

    ring->meta[tail].size = size;
    ring->meta[tail].pts_us = pts_us;
    ring->meta[tail].is_idr = is_idr;
    ring->meta[tail].sequence = ring->sequence++;

    ring->tail = (tail + 1) % ring->slot_count;
    ring->count++;

    pthread_mutex_unlock(&ring->lock);

    return 0;
}

int au_ring_pop(AuRing *ring,
                uint8_t *buffer,
                size_t buffer_capacity,
                AuMeta *meta)
{
    if (ring == NULL) {
        return 0;
    }

    pthread_mutex_lock(&ring->lock);

    if (ring->count == 0) {
        pthread_mutex_unlock(&ring->lock);
        return 0;
    }

    size_t head = ring->head;
    AuMeta m = ring->meta[head];

    int ok = m.size <= buffer_capacity;

    if (ok) {
        memcpy(buffer,
               ring->backing + head * ring->slot_capacity,
               m.size);

        if (meta != NULL) {
            *meta = m;
        }
    }

    ring->head = (head + 1) % ring->slot_count;
    ring->count--;

    pthread_mutex_unlock(&ring->lock);

    return ok ? 1 : 0;
}

uint64_t au_ring_dropped(const AuRing *ring)
{
    return ring != NULL ? ring->dropped : 0;
}

void au_ring_destroy(AuRing *ring)
{
    if (ring == NULL) {
        return;
    }

    pthread_mutex_destroy(&ring->lock);
    free(ring->backing);
    free(ring->meta);
    free(ring);
}
