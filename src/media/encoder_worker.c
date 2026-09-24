#define _POSIX_C_SOURCE 200809L

/*
 * encoder_worker.c
 *
 * Pull raw frames from the hub, convert to I420, encode and
 * publish access units to the ring.
 */
#include "encoder_worker.h"

#include "log.h"

#include <linux/videodev2.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "au_ring.h"
#include "h264_bitstream.h"
#include "yuv_convert.h"

#define ENCODE_SCRATCH_MAX (4096 * 4096 * 3 / 2)
#define AU_SCRATCH_CAPACITY (512 * 1024)
/* Room for a cached SPS + PPS in front of an access unit. */
#define AU_FIXUP_CAPACITY (AU_SCRATCH_CAPACITY + 2 * H264_PARAM_MAX)

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
    uint8_t *au_fixup;          /* access unit with SPS/PPS prepended */
    H264ParamCache params;      /* last SPS/PPS the encoder produced */

    pthread_t thread;
    atomic_int running;
    int started;

    /*
     * The HTTP thread reads these while the encode thread writes them,
     * so they are atomics. A plain uint64_t would be a data race, and a
     * race is undefined behaviour even for a counter that only grows.
     * The encode thread keeps local copies for its own logic and
     * publishes each change with a relaxed fetch_add: no ordering is
     * needed because no other state depends on these values.
     */
    atomic_uint_fast64_t frames_in;
    atomic_uint_fast64_t frames_encoded;
    atomic_uint_fast64_t skipped_idle;
    atomic_uint_fast64_t skipped_mismatch;
    atomic_uint_fast64_t skipped_bad_size;
    atomic_uint_fast64_t no_output;
    atomic_uint_fast64_t keyframes;
    atomic_uint_fast64_t params_prepended;
    atomic_int failed;          /* encoder unusable; worker stopped */

    /*
     * Live reconfiguration mailbox. The HTTP thread only stores a
     * request here; the encode thread is the one that owns the encoder
     * handle and applies it, because calling into libx264 or issuing a
     * V4L2 control ioctl while x264_encoder_encode or VIDIOC_QBUF is
     * running on the same handle is a data race. 0 means "nothing
     * pending", which is also why a bitrate request of 0 is refused.
     */
    atomic_uint pending_bitrate_kbps;
    atomic_uint applied_bitrate_kbps;
};

static void counter_add(atomic_uint_fast64_t *counter)
{
    atomic_fetch_add_explicit(counter, 1, memory_order_relaxed);
}

static uint64_t counter_get(const atomic_uint_fast64_t *counter)
{
    return (uint64_t) atomic_load_explicit(counter, memory_order_relaxed);
}

/*
 * Frames the worker may consume while active before the stall
 * watchdog complains. At 30 fps this is about three seconds,
 * well inside the ICE checking window of a connecting viewer.
 */
#define STALL_WATCHDOG_FRAMES 90

/*
 * Active time without a single access unit after which the encoder is
 * declared failed. A hung hardware encoder never returns an error, it
 * only stops producing; the server then rebuilds the pipeline (and
 * falls back to libx264 when the preference is "auto").
 */
#define STALL_FATAL_MS 10000

static uint64_t monotonic_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t) ts.tv_sec * 1000ULL + (uint64_t) ts.tv_nsec / 1000000ULL;
}

/*
 * Post-process one encoder output before it reaches the network:
 *   - a header-only buffer (SPS/PPS, no picture) is cached, not sent;
 *   - an IDR without SPS/PPS gets the cached ones prepended, so every
 *     keyframe a viewer can start from is self-contained;
 *   - the keyframe flag is taken from the NAL types, which is the
 *     authoritative source whatever the backend reported.
 * Returns 1 with the access unit in *au / *au_size, 0 when there is
 * nothing to send.
 */
static int finish_access_unit(EncoderWorker *worker, size_t size,
                              const uint8_t **au, size_t *au_size,
                              int *is_idr)
{
    H264AuInfo info;

    h264_au_scan(worker->au_buffer, size, &info);

    if (info.has_sps || info.has_pps) {
        h264_param_cache_update(&worker->params, worker->au_buffer, size);
    }

    if (!info.has_slice) {
        return 0;
    }

    *au = worker->au_buffer;
    *au_size = size;
    *is_idr = info.has_idr;

    if (info.has_idr && !(info.has_sps && info.has_pps)) {
        size_t fixed = h264_prepend_params(&worker->params, worker->au_buffer,
                                           size, worker->au_fixup,
                                           AU_FIXUP_CAPACITY);

        if (fixed > 0) {
            *au = worker->au_fixup;
            *au_size = fixed;
            counter_add(&worker->params_prepended);
        }
    }

    return 1;
}

static void *encoder_thread(void *arg)
{
    EncoderWorker *worker = arg;

    log_info("encode", "encode worker: started");

    int was_active = 0;
    int stall_warned = 0;
    int mismatch_logged = 0;
    int bad_size_logged = 0;
    int ring_full_logged = 0;
    uint64_t last_encode_at_seen = 0;
    uint64_t last_output_ms = 0;

    while (worker->running) {
        /* Sleeps until the source publishes; wakes to notice a stop. */
        Frame *frame = frame_hub_take_wait(worker->consumer, 100);

        if (frame == NULL) {
            continue;
        }

        counter_add(&worker->frames_in);

        uint64_t frames_in = counter_get(&worker->frames_in);

        /*
         * Apply a queued bitrate change now: a frame just arrived, so
         * the encoder is between calls and nothing else is touching it.
         * Done before the idle check so the change lands even while no
         * viewer is connected.
         */
        unsigned int pending_kbps =
            atomic_exchange_explicit(&worker->pending_bitrate_kbps, 0,
                                     memory_order_relaxed);

        if (pending_kbps != 0) {
            if (h264_encoder_set_bitrate(worker->encoder, pending_kbps) == 0) {
                atomic_store_explicit(&worker->applied_bitrate_kbps,
                                      pending_kbps, memory_order_relaxed);
                log_info("encode", "encode worker: bitrate now %u kbps",
                         pending_kbps);
            } else {
                log_error("encode", "encode worker: encoder refused the "
                                    "bitrate change to %u kbps",
                          pending_kbps);
            }
        }

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
            last_encode_at_seen = frames_in;
            last_output_ms = monotonic_ms();
        }
        was_active = active;

        if (!active) {
            /*
             * Idle by design: without a viewer there is nobody
             * to send the stream to, so skip conversion and
             * encoding entirely.
             */
            counter_add(&worker->skipped_idle);
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
            frames_in - last_encode_at_seen >= STALL_WATCHDOG_FRAMES) {
            stall_warned = 1;
            log_warn("encode", "encode worker: stalled, no output for %llu frames "
                    "while active (encoded total=%llu, skipped: mismatch=%llu "
                    "bad-size=%llu, encoder-no-output=%llu)",
                    (unsigned long long) (frames_in - last_encode_at_seen),
                    (unsigned long long) counter_get(&worker->frames_encoded),
                    (unsigned long long) counter_get(&worker->skipped_mismatch),
                    (unsigned long long) counter_get(&worker->skipped_bad_size),
                    (unsigned long long) counter_get(&worker->no_output));
        }

        if (frame->width != worker->width ||
            frame->height != worker->height) {
            if (!mismatch_logged) {
                mismatch_logged = 1;
                log_error("encode", "encode worker: frame %ux%u does not match the "
                        "encoder %ux%u, frames are dropped",
                        frame->width, frame->height,
                        worker->width, worker->height);
            }
            counter_add(&worker->skipped_mismatch);
            frame_unref(frame_hub_pool(worker->hub), frame);
            continue;
        }

        size_t luma = (size_t) worker->width * worker->height;
        uint32_t min_stride = frame->format == V4L2_PIX_FMT_YUYV
                                  ? worker->width * 2 : worker->width;

        if (frame->size == 0 ||
            frame->size < (size_t) (frame->stride ? frame->stride : min_stride) *
                              worker->height) {
            if (!bad_size_logged) {
                bad_size_logged = 1;
                log_info("encode", "encode worker: frame of %zu bytes is too "
                        "small for %ux%u, frames are dropped", frame->size,
                        worker->width, worker->height);
            }
            counter_add(&worker->skipped_bad_size);
            frame_unref(frame_hub_pool(worker->hub), frame);
            continue;
        }

        const uint8_t *plane_y = worker->i420;
        const uint8_t *plane_u = worker->i420 + luma;
        const uint8_t *plane_v = plane_u + luma / 4;

        if (frame->format == V4L2_PIX_FMT_YUYV) {
            yuyv_to_i420(frame->data,
                         frame->stride ? frame->stride : frame->width * 2,
                         worker->i420, worker->i420 + luma,
                         worker->i420 + luma + luma / 4,
                         worker->width, worker->height);
        } else {
            /*
             * Planar YU12: luma stride from the source, chroma at half
             * stride right after luma, as V4L2 and rpicam-vid lay it out.
             */
            uint32_t y_stride = frame->stride ? frame->stride : worker->width;
            size_t chroma_plane = (size_t) (y_stride / 2) * (worker->height / 2);
            size_t needed = (size_t) y_stride * worker->height + 2 * chroma_plane;

            if (frame->size < needed) {
                if (!bad_size_logged) {
                    bad_size_logged = 1;
                    log_info("encode", "encode worker: I420 frame of %zu bytes "
                            "is smaller than %zu, frames are dropped",
                            frame->size, needed);
                }
                counter_add(&worker->skipped_bad_size);
                frame_unref(frame_hub_pool(worker->hub), frame);
                continue;
            }

            const uint8_t *src_u = frame->data + (size_t) y_stride * worker->height;
            const uint8_t *src_v = src_u + chroma_plane;

            if (y_stride == worker->width) {
                /*
                 * Tightly packed (the CSI and stdin sources): the
                 * encoder reads the pooled frame directly, saving a
                 * full frame copy. The reference is held until the
                 * encode call returns.
                 */
                plane_y = frame->data;
                plane_u = src_u;
                plane_v = src_v;
            } else {
                i420_copy(frame->data, y_stride, src_u, src_v,
                          worker->i420, worker->i420 + luma,
                          worker->i420 + luma + luma / 4,
                          worker->width, worker->height);
            }
        }

        uint64_t pts_us = frame->timestamp_us;
        int force_idr = atomic_exchange(worker->force_idr, 0);

        size_t au_size = 0;
        int backend_idr = 0;
        uint64_t au_pts_us = pts_us;

        int result = h264_encoder_encode(worker->encoder,
                                         plane_y, plane_u, plane_v,
                                         pts_us,
                                         force_idr,
                                         worker->au_buffer,
                                         AU_SCRATCH_CAPACITY,
                                         &au_size,
                                         &backend_idr,
                                         &au_pts_us);

        /* Planes may point into the frame: release it only now. */
        frame_unref(frame_hub_pool(worker->hub), frame);

        if (result < 0) {
            log_error("encode", "encode worker: fatal encoder error, encoder "
                                "marked failed");
            atomic_store(&worker->failed, 1);
            break;
        }

        const uint8_t *au = NULL;
        size_t send_size = 0;
        int is_idr = 0;

        if (result == 0 ||
            !finish_access_unit(worker, au_size, &au, &send_size, &is_idr)) {
            /*
             * Hardware pipeline depth, a dropped frame, or a buffer
             * that only carried SPS/PPS.
             */
            counter_add(&worker->no_output);

            if (force_idr) {
                /* Not satisfied yet: keep asking. */
                atomic_store(worker->force_idr, 1);
            }

            if (last_output_ms != 0 &&
                monotonic_ms() - last_output_ms > STALL_FATAL_MS) {
                log_error("encode", "encode worker: no access unit for %d s "
                                    "while frames were supplied; encoder "
                                    "marked failed", STALL_FATAL_MS / 1000);
                atomic_store(&worker->failed, 1);
                break;
            }
            continue;
        }

        counter_add(&worker->frames_encoded);
        last_encode_at_seen = frames_in;
        last_output_ms = monotonic_ms();
        stall_warned = 0;

        if (is_idr) {
            counter_add(&worker->keyframes);
            /*
             * Satisfy pending keyframe requests that arrived
             * while this IDR was produced.
             */
            atomic_store(worker->force_idr, 0);
        } else if (force_idr) {
            /* A hardware encoder may deliver the IDR a frame later. */
            atomic_store(worker->force_idr, 1);
        }

        if (au_ring_push(worker->ring, au, send_size, au_pts_us, is_idr) != 0) {
            if (!ring_full_logged) {
                ring_full_logged = 1;
                log_warn("encode", "encode worker: %zu byte access unit does "
                                   "not fit the ring slot; dropped, keyframe "
                                   "requested", send_size);
            }
            atomic_store(worker->force_idr, 1);
        }
    }

    log_info("encode", "encode worker: stopped (%llu frames in, %llu encoded, "
                       "%llu keyframes)",
           (unsigned long long) counter_get(&worker->frames_in),
           (unsigned long long) counter_get(&worker->frames_encoded),
           (unsigned long long) counter_get(&worker->keyframes));

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
        log_info("encode", "encode worker: %ux%u exceeds the scratch limit (%lu MB)", width, height, (unsigned long) (ENCODE_SCRATCH_MAX / 1048576));
        if (worker->consumer != NULL) {
            frame_hub_unsubscribe(hub, worker->consumer);
        }
        free(worker);
        return NULL;
    }

    uint8_t *i420 = malloc(i420_size);
    uint8_t *au_buffer = malloc(AU_SCRATCH_CAPACITY);
    uint8_t *au_fixup = malloc(AU_FIXUP_CAPACITY);

    if (worker->consumer == NULL || i420 == NULL || au_buffer == NULL ||
        au_fixup == NULL) {
        if (worker->consumer != NULL) {
            frame_hub_unsubscribe(hub, worker->consumer);
        }
        free(i420);
        free(au_buffer);
        free(au_fixup);
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
    worker->au_fixup = au_fixup;
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

int encoder_worker_request_bitrate(EncoderWorker *worker, uint32_t kbps)
{
    if (worker == NULL) {
        return -1;
    }

    /*
     * The encode thread is stopped and joined during a pipeline restart;
     * ask for a non-zero value so the mailbox stays unambiguous.
     */
    if (kbps == 0) {
        return -1;
    }

    atomic_store_explicit(&worker->pending_bitrate_kbps, kbps,
                          memory_order_relaxed);

    return 0;
}

uint32_t encoder_worker_bitrate_kbps(const EncoderWorker *worker)
{
    if (worker == NULL) {
        return 0;
    }

    return (uint32_t) atomic_load_explicit(&worker->applied_bitrate_kbps,
                                           memory_order_relaxed);
}

uint64_t encoder_worker_frames_encoded(const EncoderWorker *worker)
{
    return worker != NULL ? counter_get(&worker->frames_encoded) : 0;
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

    out->frames_seen = counter_get(&worker->frames_in);
    out->frames_encoded = counter_get(&worker->frames_encoded);
    out->skipped_idle = counter_get(&worker->skipped_idle);
    out->skipped_mismatch = counter_get(&worker->skipped_mismatch);
    out->skipped_bad_size = counter_get(&worker->skipped_bad_size);
    out->no_output = counter_get(&worker->no_output);
    out->keyframes = counter_get(&worker->keyframes);
    out->params_prepended = counter_get(&worker->params_prepended);
}

int encoder_worker_failed(const EncoderWorker *worker)
{
    return worker != NULL && atomic_load(&((EncoderWorker *) worker)->failed);
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
    free(worker->au_fixup);
    free(worker);
}
