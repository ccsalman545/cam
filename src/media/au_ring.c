#define _POSIX_C_SOURCE 200809L

/*
 * au_ring.c
 */
#include "au_ring.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

struct AuRing {
    uint8_t *backing;           /* slot_count x slot_capacity */
    AuMeta *meta;
    size_t slot_capacity;
    size_t slot_count;
    size_t head;                /* next to read */
    size_t tail;                /* next to write */
    size_t count;
    int lost;                   /* next pushed AU follows a loss */
    uint64_t sequence;
    atomic_ullong dropped;      /* read by the status thread */
    int event_fd;
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
    ring->event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    atomic_init(&ring->dropped, 0);
    pthread_mutex_init(&ring->lock, NULL);

    return ring;
}

int au_ring_push(AuRing *ring,
                 const uint8_t *data,
                 size_t size,
                 uint64_t pts_us,
                 int is_idr)
{
    if (ring == NULL || data == NULL || size == 0) {
        return -1;
    }

    pthread_mutex_lock(&ring->lock);

    if (size > ring->slot_capacity) {
        /* Lost for the viewer just like an overwritten one. */
        ring->lost = 1;
        atomic_fetch_add(&ring->dropped, 1);
        pthread_mutex_unlock(&ring->lock);
        return -1;
    }

    if (ring->count == ring->slot_count) {
        /* Overwrite the oldest access unit. */
        ring->head = (ring->head + 1) % ring->slot_count;
        ring->count--;
        ring->lost = 1;
        atomic_fetch_add(&ring->dropped, 1);
    }

    size_t tail = ring->tail;

    memcpy(ring->backing + tail * ring->slot_capacity, data, size);

    ring->meta[tail].size = size;
    ring->meta[tail].pts_us = pts_us;
    ring->meta[tail].is_idr = is_idr;
    ring->meta[tail].discontinuity = ring->lost;
    ring->meta[tail].sequence = ring->sequence++;
    ring->lost = 0;

    ring->tail = (tail + 1) % ring->slot_count;
    ring->count++;

    pthread_mutex_unlock(&ring->lock);

    if (ring->event_fd >= 0) {
        uint64_t one = 1;
        ssize_t written = write(ring->event_fd, &one, sizeof(one));
        (void) written;     /* EAGAIN only when already signalled */
    }

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

    int discontinuity = 0;

    while (ring->count > 0) {
        size_t head = ring->head;
        AuMeta m = ring->meta[head];

        ring->head = (head + 1) % ring->slot_count;
        ring->count--;

        if (m.size > buffer_capacity) {
            /* Cannot be delivered: skip it, flag the gap, keep going. */
            discontinuity = 1;
            atomic_fetch_add(&ring->dropped, 1);
            continue;
        }

        memcpy(buffer, ring->backing + head * ring->slot_capacity, m.size);

        if (meta != NULL) {
            *meta = m;
            meta->discontinuity |= discontinuity;
        }

        pthread_mutex_unlock(&ring->lock);
        return 1;
    }

    /* Empty: clear the wakeup under the lock so no push is missed. */
    if (ring->event_fd >= 0) {
        uint64_t value = 0;
        ssize_t got = read(ring->event_fd, &value, sizeof(value));
        (void) got;
    }

    if (discontinuity) {
        ring->lost = 1;
    }

    pthread_mutex_unlock(&ring->lock);

    return 0;
}

uint64_t au_ring_dropped(const AuRing *ring)
{
    return ring != NULL ? atomic_load(&((AuRing *) ring)->dropped) : 0;
}

int au_ring_fd(const AuRing *ring)
{
    return ring != NULL ? ring->event_fd : -1;
}

void au_ring_destroy(AuRing *ring)
{
    if (ring == NULL) {
        return;
    }

    if (ring->event_fd >= 0) {
        close(ring->event_fd);
    }

    pthread_mutex_destroy(&ring->lock);
    free(ring->backing);
    free(ring->meta);
    free(ring);
}
