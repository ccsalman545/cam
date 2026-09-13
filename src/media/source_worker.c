#define _POSIX_C_SOURCE 200809L

/*
 * source_worker.c
 *
 * The only writer to the hub. Publishes borrowed capture
 * buffers as pooled frames.
 */
#include "source_worker.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

struct SourceWorker {
    VideoSource *source;
    FrameHub *hub;

    pthread_t thread;
    atomic_int running;
    int started;

    uint64_t captured;
};

static void *source_worker_thread(void *arg)
{
    SourceWorker *worker = arg;

    printf("source worker: started (%s, %ux%u @ %u fps)\n",
           worker->source->name,
           worker->source->width,
           worker->source->height,
           worker->source->fps);

    uint64_t sequence = 0;

    while (worker->running) {
        uint64_t timestamp_us = 0;
        const uint8_t *data = NULL;
        size_t size = 0;
        uint32_t buffer_index = 0;

        int result = worker->source->capture(worker->source,
                                             &timestamp_us,
                                             &data,
                                             &size,
                                             &buffer_index);

        if (result < 0) {
            usleep(20000);
            continue;
        }

        if (result == 0) {
            continue;
        }

        if (frame_hub_publish(worker->hub,
                              data,
                              size,
                              worker->source->width,
                              worker->source->height,
                              worker->source->format,
                              worker->source->stride,
                              sequence,
                              timestamp_us) != 0) {
            /* Pool exhausted, frame dropped. */
        }

        worker->source->release(worker->source, buffer_index);

        sequence++;
        worker->captured++;
    }

    printf("source worker: stopped (%llu frames captured)\n",
           (unsigned long long) worker->captured);

    return NULL;
}

SourceWorker *source_worker_create(VideoSource *source, FrameHub *hub)
{
    if (source == NULL || hub == NULL) {
        return NULL;
    }

    SourceWorker *worker = calloc(1, sizeof(*worker));
    if (worker == NULL) {
        return NULL;
    }

    worker->source = source;
    worker->hub = hub;
    worker->running = 1;

    return worker;
}

int source_worker_start(SourceWorker *worker)
{
    if (worker == NULL) {
        return -1;
    }

    if (worker->source->start(worker->source) != 0) {
        return -1;
    }

    if (pthread_create(&worker->thread, NULL,
                       source_worker_thread, worker) != 0) {
        return -1;
    }

    worker->started = 1;

    return 0;
}

void source_worker_stop(SourceWorker *worker)
{
    if (worker != NULL) {
        worker->running = 0;
    }
}

void source_worker_join(SourceWorker *worker)
{
    if (worker != NULL && worker->started) {
        pthread_join(worker->thread, NULL);
        worker->started = 0;
    }
}

uint64_t source_worker_captured(const SourceWorker *worker)
{
    return worker != NULL ? worker->captured : 0;
}

void source_worker_destroy(SourceWorker *worker)
{
    if (worker != NULL) {
        source_worker_join(worker);
        free(worker);
    }
}
