#define _POSIX_C_SOURCE 200809L

/*
 * source_worker.c
 *
 * The only writer to the hub. Publishes borrowed capture
 * buffers as pooled frames.
 */
#include "source_worker.h"

#include "log.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

/*
 * Consecutive capture errors before the worker stops. A USB camera
 * being unplugged fails immediately and permanently; retrying forever
 * would spin a thread and never report the fault.
 */
#define SOURCE_FATAL_ERRORS 100

struct SourceWorker {
    VideoSource *source;
    FrameHub *hub;

    pthread_t thread;
    atomic_int running;
    int started;

    /* Written by the worker thread, read by the HTTP thread. */
    atomic_ullong captured;
    atomic_ullong errors;
    atomic_int failed;
};

/*
 * Interruptible millisecond sleep. nanosleep is used instead of usleep
 * because usleep is not part of POSIX.1-2008 and is hidden by strict
 * feature test macros.
 */
static void sleep_ms(unsigned milliseconds)
{
    struct timespec delay;

    delay.tv_sec = (time_t) (milliseconds / 1000);
    delay.tv_nsec = (long) (milliseconds % 1000) * 1000000L;

    while (nanosleep(&delay, &delay) == -1 && errno == EINTR) {
        /* Signal, such as SIGTERM: sleep the remaining time. */
    }
}

static void *source_worker_thread(void *arg)
{
    SourceWorker *worker = arg;

    log_info("capture", "worker started: %s %ux%u @ %u fps",
             worker->source->name, worker->source->width,
             worker->source->height, worker->source->fps);

    uint64_t sequence = 0;
    unsigned consecutive_errors = 0;

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
            atomic_fetch_add(&worker->errors, 1);
            consecutive_errors++;

            if (consecutive_errors == 1) {
                log_error("capture", "%s: capture failed (fatal error from "
                                     "the source; see the driver message "
                                     "above)", worker->source->name);
            }

            if (consecutive_errors >= SOURCE_FATAL_ERRORS) {
                atomic_store(&worker->failed, 1);
                log_error("capture", "%s: %u consecutive capture errors, "
                                     "worker stopped. Restart the camera with "
                                     "POST /api/camera/restart",
                          worker->source->name, consecutive_errors);
                break;
            }

            sleep_ms(20);
            continue;
        }

        consecutive_errors = 0;

        if (result == 0) {
            continue;
        }

        /*
         * A failed publish means every pooled buffer is still
         * referenced: the consumer is behind, so this frame is dropped
         * rather than queued. Freshness beats completeness.
         */
        frame_hub_publish(worker->hub,
                          data,
                          size,
                          worker->source->width,
                          worker->source->height,
                          worker->source->format,
                          worker->source->stride,
                          sequence,
                          timestamp_us);

        worker->source->release(worker->source, buffer_index);

        sequence++;
        atomic_fetch_add(&worker->captured, 1);
    }

    log_info("capture", "worker stopped: %llu frames captured, %llu errors",
             (unsigned long long) atomic_load(&worker->captured),
             (unsigned long long) atomic_load(&worker->errors));

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
    return worker != NULL ? atomic_load(&worker->captured) : 0;
}

uint64_t source_worker_errors(const SourceWorker *worker)
{
    return worker != NULL ? atomic_load(&worker->errors) : 0;
}

int source_worker_failed(const SourceWorker *worker)
{
    return worker != NULL && atomic_load(&worker->failed);
}

void source_worker_destroy(SourceWorker *worker)
{
    if (worker != NULL) {
        source_worker_join(worker);
        free(worker);
    }
}
