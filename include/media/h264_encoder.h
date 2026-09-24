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
 *   - H264_ENCODER_SW: libx264, superfast preset, zerolatency tune,
 *     sliced threads.
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
 * width for luma and width/2 for chroma.
 *
 * Returns:
 *   1  access unit written to out, size in *out_size, its capture
 *      timestamp in *out_pts_us (a hardware encoder may return the
 *      picture of an earlier call; the timestamp is that picture's)
 *   0  no output this call (hardware pipeline depth, or a frame the
 *      backend had to drop; it then forces the next one to be an IDR)
 *  -1  fatal encoder error: the handle is unusable
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
                        int *out_is_idr,
                        uint64_t *out_pts_us);

/*
 * Options for h264_encoder_open_flags().
 *
 * H264_ENCODER_SINGLE_SLICE  every picture is coded as one slice. The
 *     libpeer RTP packetizer treats each slice NAL unit as a complete
 *     frame (marker bit and timestamp step per slice), so a multi-slice
 *     picture would reach the browser as several broken frames. libx264
 *     then runs one encode thread instead of sliced threads (frame
 *     threads are not an option: each adds a frame of latency). The
 *     V4L2 M2M encoder always emits one slice per picture.
 */
#define H264_ENCODER_SINGLE_SLICE 0x1u

/* As h264_encoder_open(), with H264_ENCODER_* option flags. */
H264Encoder *h264_encoder_open_flags(const char *preference,
                                     uint32_t width,
                                     uint32_t height,
                                     uint32_t fps,
                                     uint32_t bitrate_kbps,
                                     uint32_t gop_seconds,
                                     unsigned flags,
                                     char *name_out,
                                     size_t name_out_size);

/* Backend kind of an open encoder. */
H264EncoderKind h264_encoder_kind(const H264Encoder *encoder);

/*
 * Change the target bitrate of a running encoder. Returns 0 when the
 * backend accepted the new rate and -1 when it cannot (the caller then
 * reports the setting as requiring a restart). The picture size and
 * frame rate cannot change on a live encoder.
 *
 * Only the thread that owns the encoder may call this: reconfiguring
 * libx264 or issuing a V4L2 control ioctl concurrently with encoding on
 * the same handle is a data race. The HTTP thread goes through
 * encoder_worker_request_bitrate() instead.
 */
int h264_encoder_set_bitrate(H264Encoder *encoder, uint32_t bitrate_kbps);

void h264_encoder_close(H264Encoder *encoder);

#endif
