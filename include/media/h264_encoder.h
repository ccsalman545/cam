/*
 * h264_encoder.h
 *
 * Encoder abstraction for the WebRTC video track.
 *
 * Backends:
 *   - H264_ENCODER_HW: V4L2 stateful memory-to-memory encoder.
 *     On Raspberry Pi this is the bcm2835-codec H.264 encoder
 *     (usually /dev/video11), which offloads all encoding work
 *     to the GPU or hardware block.
 *   - H264_ENCODER_SW: libx264 in zerolatency tune.
 *
 * The factory preference string selects behavior:
 *   "auto"            try hardware first, fall back to software
 *   "hw"              first detected V4L2 M2M encoder
 *   "hw:/dev/video11" one specific device
 *   "sw"              libx264 only
 *
 * All backends consume planar I420 frames and emit Annex-B
 * H.264 access units.
 */
#ifndef MEDIA_H264_ENCODER_H
#define MEDIA_H264_ENCODER_H

#include <stddef.h>
#include <stdint.h>

typedef struct H264Encoder H264Encoder;

typedef enum {
    H264_ENCODER_HW = 1,
    H264_ENCODER_SW = 2
} H264EncoderKind;

/*
 * Open an encoder. On success the selected backend name is
 * written to name_out (for logs and /status). Returns NULL on
 * failure.
 */
H264Encoder *h264_encoder_open(const char *preference,
                               uint32_t width,
                               uint32_t height,
                               uint32_t fps,
                               uint32_t bitrate_kbps,
                               uint32_t gop_seconds,
                               char *name_out,
                               size_t name_out_size);

/*
 * Encode one I420 frame. Plane strides are assumed equal to
 * width for luma and width/2 for chroma (the caller normalizes
 * through yuv_convert).
 *
 * Returns:
 *   1  access unit written to out, size in *out_size
 *   0  no output ready yet (hardware pipeline depth)
 *  -1  fatal encoder error
 */
int h264_encoder_encode(H264Encoder *encoder,
                        const uint8_t *plane_y,
                        const uint8_t *plane_u,
                        const uint8_t *plane_v,
                        uint64_t pts_us,
                        int force_idr,
                        uint8_t *out,
                        size_t out_capacity,
                        size_t *out_size,
                        int *out_is_idr);

void h264_encoder_close(H264Encoder *encoder);

#endif
