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

uint64_t encoder_worker_frames_encoded(const EncoderWorker *worker);

void encoder_worker_destroy(EncoderWorker *worker);

#endif
