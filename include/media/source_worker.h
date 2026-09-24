/*
 * source_worker.h
 *
 * Capture thread: drives a VideoSource and publishes every
 * frame into the frame hub.
 */
#ifndef MEDIA_SOURCE_WORKER_H
#define MEDIA_SOURCE_WORKER_H

#include <stdint.h>

#include "frame_hub.h"
#include "video_source.h"

typedef struct SourceWorker SourceWorker;

SourceWorker *source_worker_create(VideoSource *source, FrameHub *hub);

int source_worker_start(SourceWorker *worker);

void source_worker_stop(SourceWorker *worker);

/*
 * Block until the thread exited. Shutdown may take up to one
 * camera poll timeout (200 ms) plus one frame period.
 */
void source_worker_join(SourceWorker *worker);

/* Frames published into the hub since start. */
uint64_t source_worker_captured(const SourceWorker *worker);

/*
 * Capture errors reported by the source (capture() returned -1) and
 * whether the worker gave up. A camera that disappears keeps returning
 * errors; after SOURCE_FATAL_ERRORS in a row the worker stops trying
 * and marks itself failed so /api/status can say so instead of showing
 * a healthy process with no video.
 */
uint64_t source_worker_errors(const SourceWorker *worker);

int source_worker_failed(const SourceWorker *worker);

void source_worker_destroy(SourceWorker *worker);

#endif
