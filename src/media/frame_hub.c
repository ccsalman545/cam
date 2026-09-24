/*
 * frame_hub.c
 *
 * See frame_hub.h for the design rationale.
 */
#define _POSIX_C_SOURCE 200809L

#include "frame_hub.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct FrameHubConsumer {
    FrameHub *hub;
    Frame *slot;
    pthread_mutex_t lock;
    pthread_cond_t ready;           /* signalled when slot is filled */
    int active;
    struct FrameHubConsumer *next;
    struct FrameHubConsumer *prev;
};

struct FrameHub {
    FramePool *pool;
    pthread_mutex_t lock;           /* guards the consumer list */
    FrameHubConsumer *consumers;
};

FrameHub *frame_hub_create(size_t buffer_capacity, size_t pool_count)
{
    FrameHub *hub = calloc(1, sizeof(*hub));
    if (hub == NULL) {
        return NULL;
    }

    hub->pool = frame_pool_create(buffer_capacity, pool_count);
    if (hub->pool == NULL) {
        free(hub);
        return NULL;
    }

    pthread_mutex_init(&hub->lock, NULL);

    return hub;
}

FrameHubConsumer *frame_hub_subscribe(FrameHub *hub)
{
    if (hub == NULL) {
        return NULL;
    }

    FrameHubConsumer *consumer = calloc(1, sizeof(*consumer));
    if (consumer == NULL) {
        return NULL;
    }

    consumer->hub = hub;
    consumer->active = 1;
    pthread_mutex_init(&consumer->lock, NULL);

    /* Timed waits run on the monotonic clock: immune to clock steps. */
    pthread_condattr_t attr;

    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&consumer->ready, &attr);
    pthread_condattr_destroy(&attr);

    pthread_mutex_lock(&hub->lock);

    consumer->next = hub->consumers;
    if (hub->consumers != NULL) {
        hub->consumers->prev = consumer;
    }
    hub->consumers = consumer;

    pthread_mutex_unlock(&hub->lock);

    return consumer;
}

void frame_hub_unsubscribe(FrameHub *hub, FrameHubConsumer *consumer)
{
    if (hub == NULL || consumer == NULL) {
        return;
    }

    pthread_mutex_lock(&hub->lock);
    pthread_mutex_lock(&consumer->lock);

    if (consumer->active) {
        consumer->active = 0;

        if (consumer->prev != NULL) {
            consumer->prev->next = consumer->next;
        } else {
            hub->consumers = consumer->next;
        }
        if (consumer->next != NULL) {
            consumer->next->prev = consumer->prev;
        }
    }

    pthread_mutex_unlock(&consumer->lock);
    pthread_mutex_unlock(&hub->lock);

    if (consumer->slot != NULL) {
        frame_unref(hub->pool, consumer->slot);
        consumer->slot = NULL;
    }

    pthread_cond_destroy(&consumer->ready);
    pthread_mutex_destroy(&consumer->lock);
    free(consumer);
}

int frame_hub_publish(FrameHub *hub,
                      const uint8_t *data,
                      size_t size,
                      uint32_t width,
                      uint32_t height,
                      uint32_t format,
                      uint32_t stride,
                      uint64_t sequence,
                      uint64_t timestamp_us)
{
    if (hub == NULL || data == NULL || size == 0) {
        return -1;
    }

    Frame *frame = frame_pool_acquire(hub->pool);
    if (frame == NULL) {
        return -1;
    }

    memcpy(frame->data, data, size);

    frame->width = width;
    frame->height = height;
    frame->format = format;
    frame->stride = stride;
    frame->size = size;
    frame->sequence = sequence;
    frame->timestamp_us = timestamp_us;

    pthread_mutex_lock(&hub->lock);

    /*
     * Deliver to every consumer mailbox. Keep-newest policy.
     */
    for (FrameHubConsumer *c = hub->consumers; c != NULL; c = c->next) {
        pthread_mutex_lock(&c->lock);

        if (!c->active) {
            pthread_mutex_unlock(&c->lock);
            continue;
        }

        frame_ref(frame);

        if (c->slot != NULL) {
            /*
             * Previous frame was never taken. Drop it so the
             * consumer always sees the newest frame.
             */
            frame_unref(hub->pool, c->slot);
        }

        c->slot = frame;

        pthread_cond_signal(&c->ready);
        pthread_mutex_unlock(&c->lock);
    }

    pthread_mutex_unlock(&hub->lock);

    /*
     * Drop the publisher reference. If nobody subscribed the
     * buffer returns to the pool immediately.
     */
    frame_unref(hub->pool, frame);

    return 0;
}

Frame *frame_hub_take(FrameHubConsumer *consumer)
{
    if (consumer == NULL) {
        return NULL;
    }

    pthread_mutex_lock(&consumer->lock);

    Frame *frame = consumer->slot;
    consumer->slot = NULL;

    pthread_mutex_unlock(&consumer->lock);

    return frame;
}

Frame *frame_hub_take_wait(FrameHubConsumer *consumer, int timeout_ms)
{
    if (consumer == NULL) {
        return NULL;
    }

    struct timespec deadline;

    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long) (timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&consumer->lock);

    while (consumer->slot == NULL) {
        if (pthread_cond_timedwait(&consumer->ready, &consumer->lock,
                                   &deadline) == ETIMEDOUT) {
            break;
        }
    }

    Frame *frame = consumer->slot;
    consumer->slot = NULL;

    pthread_mutex_unlock(&consumer->lock);

    return frame;
}

FramePool *frame_hub_pool(FrameHub *hub)
{
    return hub != NULL ? hub->pool : NULL;
}

void frame_hub_destroy(FrameHub *hub)
{
    if (hub == NULL) {
        return;
    }

    while (hub->consumers != NULL) {
        frame_hub_unsubscribe(hub, hub->consumers);
    }

    frame_pool_destroy(hub->pool);
    pthread_mutex_destroy(&hub->lock);
    free(hub);
}
