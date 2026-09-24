/*
 * frame_pool.c
 *
 * See frame_pool.h for the pool contract.
 */
#include "log.h"

#include "frame_pool.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct FramePool {
    Frame *frames;          /* count entries */
    uint8_t *backing;       /* count * capacity bytes */
    size_t capacity;
    size_t count;
    int *free_list;
    size_t free_count;
    pthread_mutex_t lock;
};

FramePool *frame_pool_create(size_t capacity, size_t count)
{
    if (capacity == 0 || count == 0) {
        return NULL;
    }

    FramePool *pool = calloc(1, sizeof(*pool));
    if (pool == NULL) {
        return NULL;
    }

    pool->frames = calloc(count, sizeof(Frame));
    pool->backing = calloc(count, capacity);
    pool->free_list = calloc(count, sizeof(int));

    if (pool->frames == NULL || pool->backing == NULL ||
        pool->free_list == NULL) {
        free(pool->frames);
        free(pool->backing);
        free(pool->free_list);
        free(pool);
        return NULL;
    }

    pthread_mutex_init(&pool->lock, NULL);

    pool->capacity = capacity;
    pool->count = count;

    for (size_t i = 0; i < count; i++) {
        pool->frames[i].data = pool->backing + i * capacity;
        pool->frames[i].pool_index = (int) i;
        pool->free_list[i] = (int) i;
    }
    pool->free_count = count;

    return pool;
}

Frame *frame_pool_acquire(FramePool *pool)
{
    if (pool == NULL) {
        return NULL;
    }

    pthread_mutex_lock(&pool->lock);

    if (pool->free_count == 0) {
        pthread_mutex_unlock(&pool->lock);
        return NULL;
    }

    int index = pool->free_list[--pool->free_count];

    pthread_mutex_unlock(&pool->lock);

    Frame *frame = &pool->frames[index];

    memset(frame, 0, offsetof(Frame, data));

    frame->data = pool->backing + (size_t) index * pool->capacity;
    frame->pool_index = index;
    frame->refcount = 1;

    return frame;
}

void frame_ref(Frame *frame)
{
    if (frame == NULL) {
        return;
    }

    /*
     * Reference counts are only mutated while a hub lock or
     * pool lock is held by the caller in this codebase, but a
     * plain atomic-free increment is still safe here because
     * every publisher/consumer path serializes through the
     * pool or hub mutex before touching refcount.
     */
    frame->refcount++;
}

void frame_unref(FramePool *pool, Frame *frame)
{
    if (pool == NULL || frame == NULL) {
        return;
    }

    pthread_mutex_lock(&pool->lock);

    if (frame->refcount <= 0) {
        log_error("media", "frame_pool: double unref detected (index %d)",
                  frame->pool_index);
        pthread_mutex_unlock(&pool->lock);
        return;
    }

    frame->refcount--;

    if (frame->refcount <= 0 && frame->pool_index >= 0) {
        if (pool->free_count >= pool->count) {
            log_error("media", "frame_pool: free list overflow");
        } else {
            pool->free_list[pool->free_count++] = frame->pool_index;
        }
        frame->pool_index = -1;
    }

    pthread_mutex_unlock(&pool->lock);
}

void frame_pool_destroy(FramePool *pool)
{
    if (pool == NULL) {
        return;
    }

    pthread_mutex_destroy(&pool->lock);

    free(pool->frames);
    free(pool->backing);
    free(pool->free_list);
    free(pool);
}
