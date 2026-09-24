/*
 * encoder_worker.h
 *
 * Encode thread: consumes raw frames from the hub, converts
 * them to I420 and pushes H.264 access units into the ring
 * that the network thread drains.
 *
 * The worker is CPU adaptive: when no WebRTC session is
 * active it drops frames immediately and skips conversion and
 * encoding, so idle operation costs almost nothing (important
 * on Raspberry Pi).
 */
#ifndef MEDIA_ENCODER_WORKER_H
#define MEDIA_ENCODER_WORKER_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#include "au_ring.h"
#include "frame_hub.h"
#include "h264_encoder.h"

typedef struct EncoderWorker EncoderWorker;

EncoderWorker *encoder_worker_create(FrameHub *hub,
                                     H264Encoder *encoder,
                                     uint32_t width,
                                     uint32_t height,
                                     AuRing *ring,
                                     atomic_int *force_idr,
                                     const atomic_int *active);

int encoder_worker_start(EncoderWorker *worker);

void encoder_worker_stop(EncoderWorker *worker);

void encoder_worker_join(EncoderWorker *worker);

/*
 * Queue a target bitrate change for the encode thread. Returns 0 when the
 * request was queued, -1 when there is no worker or the value is 0.
 *
 * The caller must not call into the encoder itself: the encode thread owns
 * the encoder handle, and libx264 reconfiguration is not safe to run
 * concurrently with x264_encoder_encode.
 */
int encoder_worker_request_bitrate(EncoderWorker *worker, uint32_t kbps);

/* Bitrate the encode thread last applied, 0 when none has been applied. */
uint32_t encoder_worker_bitrate_kbps(const EncoderWorker *worker);

uint64_t encoder_worker_frames_encoded(const EncoderWorker *worker);

/*
 * Breakdown of what the encode thread did with the frames it
 * pulled from the hub. Lets /status tell "idle by design"
 * (skipped_idle == everything) apart from a real stall
 * (mismatch, bad size or encoder producing no output).
 */
typedef struct {
    uint64_t frames_seen;       /* frames taken from the hub */
    uint64_t frames_encoded;    /* access units pushed into the ring */
    uint64_t skipped_idle;      /* dropped: no WebRTC session active */
    uint64_t skipped_mismatch;  /* dropped: width/height not as expected */
    uint64_t skipped_bad_size;  /* dropped: empty frame (size 0) */
    uint64_t no_output;         /* encode call returned nothing */
} EncoderWorkerStats;

void encoder_worker_get_stats(const EncoderWorker *worker,
                              EncoderWorkerStats *out);

void encoder_worker_destroy(EncoderWorker *worker);

#endif
