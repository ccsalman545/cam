#define _POSIX_C_SOURCE 200809L

/*
 * encoder_worker.c
 *
 * Pull raw frames from the hub, convert to I420, encode and
 * publish access units to the ring.
 */
#include "encoder_worker.h"

#include <linux/videodev2.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "au_ring.h"
#include "yuv_convert.h"

#define ENCODE_SCRATCH_MAX (4096 * 4096 * 3 / 2)
#define AU_SCRATCH_CAPACITY (512 * 1024)

struct EncoderWorker {
    FrameHub *hub;
    FrameHubConsumer *consumer;
    H264Encoder *encoder;
    AuRing *ring;
    atomic_int *force_idr;
    const atomic_int *active;

    uint32_t width;
    uint32_t height;

    uint8_t *i420;              /* planar scratch: y, then u, then v */
    uint8_t *au_buffer;         /* access unit output */

    pthread_t thread;
    atomic_int running;
    int started;

    uint64_t frames_in;
    uint64_t frames_encoded;
    uint64_t skipped_idle;
    uint64_t skipped_mismatch;
    uint64_t skipped_bad_size;
    uint64_t no_output;
};

/*
 * Frames the worker may consume while active before the stall
 * watchdog complains. At 30 fps this is about three seconds,
 * well inside the ICE checking window of a connecting viewer.
 */
#define STALL_WATCHDOG_FRAMES 90

static void *encoder_thread(void *arg)
{
    EncoderWorker *worker = arg;

    printf("encode worker: started\n");

    int was_active = 0;
    int stall_warned = 0;
    int mismatch_logged = 0;
    int bad_size_logged = 0;
    uint64_t last_encode_at_seen = 0;

    while (worker->running) {
        Frame *frame = frame_hub_take(worker->consumer);

        if (frame == NULL) {
            /*
             * Nothing new: brief sleep keeps this well below
             * one percent CPU.
             */
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 2000000 };
            nanosleep(&ts, NULL);
            continue;
        }

        worker->frames_in++;

        int active = 0;
        if (worker->active != NULL) {
            active = atomic_load(worker->active);
        }

        if (active && !was_active) {
            /*
             * A viewer session appeared: re-arm the stall
             * watchdog for this active window.
             */
            stall_warned = 0;
            last_encode_at_seen = worker->frames_in;
        }
        was_active = active;

        if (!active) {
            /*
             * Idle by design: without a viewer there is nobody
             * to send the stream to, so skip conversion and
             * encoding entirely.
             */
            worker->skipped_idle++;
            frame_unref(frame_hub_pool(worker->hub), frame);
            continue;
        }

        /*
         * Stall watchdog: active for a while without a single
         * encoded access unit. Report once with the full drop
         * breakdown so the cause is identifiable; re-arms when
         * the encoder produces output again.
         */
        if (!stall_warned &&
            worker->frames_in - last_encode_at_seen >= STALL_WATCHDOG_FRAMES) {
            stall_warned = 1;
            fprintf(stderr,
                    "encode worker: stalled, no output for %llu frames while "
                    "active (encoded total=%llu, skipped: mismatch=%llu "
                    "bad-size=%llu, encoder-no-output=%llu)\n",
                    (unsigned long long) (worker->frames_in -
                                          last_encode_at_seen),
                    (unsigned long long) worker->frames_encoded,
                    (unsigned long long) worker->skipped_mismatch,
                    (unsigned long long) worker->skipped_bad_size,
                    (unsigned long long) worker->no_output);
        }

        if (frame->width != worker->width ||
            frame->height != worker->height) {
            if (!mismatch_logged) {
                mismatch_logged = 1;
                fprintf(stderr,
                        "encode worker: frame %ux%u does not match the "
                        "encoder %ux%u, frames are dropped\n",
                        frame->width, frame->height,
                        worker->width, worker->height);
            }
            worker->skipped_mismatch++;
            frame_unref(frame_hub_pool(worker->hub), frame);
            continue;
        }

        if (frame->size == 0) {
            if (!bad_size_logged) {
                bad_size_logged = 1;
                fprintf(stderr,
                        "encode worker: empty frame (size 0), "
                        "frames are dropped\n");
            }
            worker->skipped_bad_size++;
            frame_unref(frame_hub_pool(worker->hub), frame);
            continue;
        }

        /*
         * Convert to planar I420 in the scratch buffer.
         */
        uint8_t *plane_y = worker->i420;
        uint8_t *plane_u = worker->i420 +
                           (size_t) worker->width * worker->height;
        uint8_t *plane_v = plane_u +
                           (size_t) worker->width * worker->height / 4;

        if (frame->format == V4L2_PIX_FMT_YUYV) {
            yuyv_to_i420(frame->data,
                         frame->stride ? frame->stride : frame->width * 2,
                         plane_y, plane_u, plane_v,
                         worker->width, worker->height);
        } else {
            /*
             * Planar YU12 source. Luma stride from the driver,
             * chroma assumed at half stride right after luma.
             */
            uint32_t y_stride = frame->stride ? frame->stride : frame->width;
            const uint8_t *src_y = frame->data;
            const uint8_t *src_u = frame->data +
                                   (size_t) y_stride * frame->height;
            const uint8_t *src_v = src_u +
                                   (size_t) (y_stride / 2) * (frame->height / 2);

            i420_copy(src_y, y_stride, src_u, src_v,
                      plane_y, plane_u, plane_v,
                      worker->width, worker->height);
        }

        /* The frame may be recycled immediately after unref; retain all
         * metadata needed by the encoded access unit before releasing it. */
        uint64_t pts_us = frame->timestamp_us;
        int force_idr = atomic_exchange(worker->force_idr, 0);

        size_t au_size = 0;
        int is_idr = 0;

        int result = h264_encoder_encode(worker->encoder,
                                         plane_y, plane_u, plane_v,
                                         pts_us,
                                         force_idr,
                                         worker->au_buffer,
                                         AU_SCRATCH_CAPACITY,
                                         &au_size,
                                         &is_idr);

        frame_unref(frame_hub_pool(worker->hub), frame);

        if (result < 0) {
            fprintf(stderr, "encode worker: encoder error, stopping\n");
            break;
        }

        if (result == 0) {
            /*
             * Hardware pipeline depth: no output this round.
             */
            worker->no_output++;
            continue;
        }

        worker->frames_encoded++;
        last_encode_at_seen = worker->frames_in;
        stall_warned = 0;

        if (is_idr) {
            /*
             * Satisfy pending keyframe requests that arrived
             * while this IDR was produced.
             */
            atomic_store(worker->force_idr, 0);
        }

        if (au_ring_push(worker->ring,
                         worker->au_buffer,
                         au_size,
                         pts_us,
                         is_idr) != 0) {
            fprintf(stderr, "encode worker: access unit too large\n");
        }
    }

    printf("encode worker: stopped (%llu frames in, %llu encoded)\n",
           (unsigned long long) worker->frames_in,
           (unsigned long long) worker->frames_encoded);

    return NULL;
}

EncoderWorker *encoder_worker_create(FrameHub *hub,
                                     H264Encoder *encoder,
                                     uint32_t width,
                                     uint32_t height,
                                     AuRing *ring,
                                     atomic_int *force_idr,
                                     const atomic_int *active)
{
    EncoderWorker *worker = calloc(1, sizeof(*worker));
    if (worker == NULL) {
        return NULL;
    }

    worker->consumer = frame_hub_subscribe(hub);

    size_t i420_size = (size_t) width * height * 3 / 2;

    if (i420_size > ENCODE_SCRATCH_MAX) {
        fprintf(stderr,
                "encode worker: %ux%u exceeds the scratch limit (%lu MB)\n",
                width, height, (unsigned long) (ENCODE_SCRATCH_MAX / 1048576));
        if (worker->consumer != NULL) {
            frame_hub_unsubscribe(hub, worker->consumer);
        }
        free(worker);
        return NULL;
    }

    uint8_t *i420 = malloc(i420_size);
    uint8_t *au_buffer = malloc(AU_SCRATCH_CAPACITY);

    if (worker->consumer == NULL || i420 == NULL || au_buffer == NULL) {
        if (worker->consumer != NULL) {
            frame_hub_unsubscribe(hub, worker->consumer);
        }
        free(i420);
        free(au_buffer);
        free(worker);
        return NULL;
    }

    worker->hub = hub;
    worker->encoder = encoder;
    worker->ring = ring;
    worker->force_idr = force_idr;
    worker->active = active;
    worker->width = width;
    worker->height = height;
    worker->i420 = i420;
    worker->au_buffer = au_buffer;
    worker->running = 1;

    return worker;
}

int encoder_worker_start(EncoderWorker *worker)
{
    if (worker == NULL) {
        return -1;
    }

    if (pthread_create(&worker->thread, NULL, encoder_thread, worker) != 0) {
        return -1;
    }

    worker->started = 1;
    return 0;
}

void encoder_worker_stop(EncoderWorker *worker)
{
    if (worker != NULL) {
        worker->running = 0;
    }
}

void encoder_worker_join(EncoderWorker *worker)
{
    if (worker != NULL && worker->started) {
        pthread_join(worker->thread, NULL);
        worker->started = 0;
    }
}

uint64_t encoder_worker_frames_encoded(const EncoderWorker *worker)
{
    return worker != NULL ? worker->frames_encoded : 0;
}

void encoder_worker_get_stats(const EncoderWorker *worker,
                              EncoderWorkerStats *out)
{
    if (out == NULL) {
        return;
    }

    if (worker == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }

    /*
     * The counters are written by the encode thread only and
     * only ever grow, so a plain read from the HTTP thread is
     * benign here (same contract as frames_encoded above).
     */
    out->frames_seen = worker->frames_in;
    out->frames_encoded = worker->frames_encoded;
    out->skipped_idle = worker->skipped_idle;
    out->skipped_mismatch = worker->skipped_mismatch;
    out->skipped_bad_size = worker->skipped_bad_size;
    out->no_output = worker->no_output;
}

void encoder_worker_destroy(EncoderWorker *worker)
{
    if (worker == NULL) {
        return;
    }

    encoder_worker_join(worker);

    frame_hub_unsubscribe(worker->hub, worker->consumer);
    free(worker->i420);
    free(worker->au_buffer);
    free(worker);
}
