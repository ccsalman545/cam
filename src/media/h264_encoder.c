#define _POSIX_C_SOURCE 200809L

/*
 * h264_encoder.c
 *
 * Encoder factory: resolves the preference string to a backend
 * and dispatches encode/close calls.
 *
 * Preference resolution:
 *
 *   "auto"            scan /dev/videoN for a V4L2 M2M H.264
 *                     encoder, fall back to libx264
 *   "hw"              first detected hardware encoder, no
 *                     fallback
 *   "hw:/dev/videoNN" one explicit device, no fallback
 *   "sw"              libx264 only
 */
#include "h264_encoder.h"

#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * encoder_v4l2m2m.c
 */
void *m2m_backend_open(const char *path,
                       uint32_t width, uint32_t height,
                       uint32_t fps, uint32_t bitrate_kbps,
                       uint32_t gop_seconds,
                       char *name_out, size_t name_out_size);
int m2m_backend_probe(const char *path);
int m2m_backend_encode(void *backend,
                       const uint8_t *y, const uint8_t *u, const uint8_t *v,
                       uint64_t pts_us, int force_idr,
                       uint8_t *out, size_t out_capacity,
                       size_t *out_size, int *out_is_idr,
                       uint64_t *out_pts_us);
int m2m_backend_set_bitrate(void *backend, uint32_t bitrate_kbps);
void m2m_backend_close(void *backend);

/*
 * encoder_x264.c (linked only when HAVE_X264 is set)
 */
#if HAVE_X264
void *x264_backend_open(uint32_t width, uint32_t height,
                        uint32_t fps, uint32_t bitrate_kbps,
                        uint32_t gop_seconds, int single_slice,
                        char *name_out, size_t name_out_size);
int x264_backend_encode(void *backend,
                        const uint8_t *y, const uint8_t *u, const uint8_t *v,
                        uint64_t pts_us, int force_idr,
                        uint8_t *out, size_t out_capacity,
                        size_t *out_size, int *out_is_idr,
                        uint64_t *out_pts_us);
int x264_backend_set_bitrate(void *backend, uint32_t bitrate_kbps);
void x264_backend_close(void *backend);
#endif

struct H264Encoder {
    void *backend;
    int (*encode)(void *backend,
                  const uint8_t *y, const uint8_t *u, const uint8_t *v,
                  uint64_t pts_us, int force_idr,
                  uint8_t *out, size_t out_capacity,
                  size_t *out_size, int *out_is_idr,
                  uint64_t *out_pts_us);
    int (*set_bitrate)(void *backend, uint32_t bitrate_kbps);
    void (*close)(void *backend);
    H264EncoderKind kind;
};

#define MAX_VIDEO_DEVICE 64

/*
 * The Raspberry Pi encoder is /dev/video11 on every Pi up to the 4, so
 * it is probed first; the scan covers other boards and renumbered
 * nodes. A Pi 5 has no H.264 encoder block at all.
 */
static int find_m2m_device(char *path_out, size_t path_size)
{
    char path[32];

    if (m2m_backend_probe("/dev/video11")) {
        snprintf(path_out, path_size, "/dev/video11");
        return 0;
    }

    for (unsigned int i = 0; i <= MAX_VIDEO_DEVICE; i++) {
        if (i == 11) {
            continue;
        }

        snprintf(path, sizeof(path), "/dev/video%u", i);

        if (m2m_backend_probe(path)) {
            snprintf(path_out, path_size, "%s", path);
            return 0;
        }
    }

    return -1;
}

H264Encoder *h264_encoder_open(const char *preference,
                               uint32_t width,
                               uint32_t height,
                               uint32_t fps,
                               uint32_t bitrate_kbps,
                               uint32_t gop_seconds,
                               char *name_out,
                               size_t name_out_size)
{
    return h264_encoder_open_flags(preference, width, height, fps,
                                   bitrate_kbps, gop_seconds, 0, name_out,
                                   name_out_size);
}

H264Encoder *h264_encoder_open_flags(const char *preference,
                                     uint32_t width,
                                     uint32_t height,
                                     uint32_t fps,
                                     uint32_t bitrate_kbps,
                                     uint32_t gop_seconds,
                                     unsigned flags,
                                     char *name_out,
                                     size_t name_out_size)
{
    (void) flags;   /* only libx264 has options; unused without it */

    if (preference == NULL) {
        preference = "auto";
    }

    if (width % 2 != 0 || height % 2 != 0 || fps == 0) {
        log_error("encode", "even dimensions and a nonzero frame rate are "
                            "required (requested %ux%u @ %u)", width, height,
                  fps);
        return NULL;
    }

    int want_hw = 0;
    int want_sw = 0;
    const char *explicit_device = NULL;

    if (strcmp(preference, "auto") == 0) {
        want_hw = 1;
        want_sw = 1;
    } else if (strcmp(preference, "hw") == 0) {
        want_hw = 1;
    } else if (strcmp(preference, "sw") == 0) {
        want_sw = 1;
    } else if (strncmp(preference, "hw:", 3) == 0 && preference[3] != 0) {
        want_hw = 1;
        explicit_device = preference + 3;
    } else {
        log_error("encode", "unknown encoder preference '%s' (use auto, hw, "
                            "hw:/dev/videoNN or sw)", preference);
        return NULL;
    }

    char selected[80] = "";

    /*
     * Hardware attempt.
     */
    if (want_hw) {
        char device_path[32];
        const char *path = explicit_device;

        if (path == NULL) {
            if (find_m2m_device(device_path, sizeof(device_path)) != 0) {
                if (!want_sw) {
                    log_error("encode", "no V4L2 M2M H.264 encoder detected "
                                        "(a Raspberry Pi 5 has none; use "
                                        "--encoder sw)");
                } else {
                    log_info("encode", "no V4L2 M2M H.264 encoder detected "
                                       "(normal on a Raspberry Pi 5), using "
                                       "libx264");
                }
            } else {
                path = device_path;
            }
        }

        if (path != NULL) {
            void *backend = m2m_backend_open(path, width, height, fps,
                                             bitrate_kbps, gop_seconds,
                                             selected, sizeof(selected));

            if (backend != NULL) {
                H264Encoder *encoder = calloc(1, sizeof(*encoder));
                if (encoder == NULL) {
                    m2m_backend_close(backend);
                    return NULL;
                }

                encoder->backend = backend;
                encoder->encode = m2m_backend_encode;
                encoder->set_bitrate = m2m_backend_set_bitrate;
                encoder->close = m2m_backend_close;
                encoder->kind = H264_ENCODER_HW;

                log_info("encode", "hardware encoder: %s", selected);

                if (name_out != NULL && name_out_size > 0) {
                    snprintf(name_out, name_out_size, "%s", selected);
                }

                return encoder;
            }

            if (explicit_device != NULL || !want_sw) {
                log_error("encode", "hardware encoder %s failed to open (see "
                                    "the m2m message above)", path);
                return NULL;
            }

            log_warn("encode", "hardware encoder %s failed to open (see the "
                               "m2m message above); falling back to libx264",
                     path);
        }
    }

    /*
     * Software fallback.
     */
#if HAVE_X264
    if (want_sw) {
        void *backend = x264_backend_open(width, height, fps,
                                          bitrate_kbps, gop_seconds,
                                          (flags & H264_ENCODER_SINGLE_SLICE)
                                              != 0,
                                          selected, sizeof(selected));

        if (backend != NULL) {
            H264Encoder *encoder = calloc(1, sizeof(*encoder));
            if (encoder == NULL) {
                x264_backend_close(backend);
                return NULL;
            }

            encoder->backend = backend;
            encoder->encode = x264_backend_encode;
            encoder->set_bitrate = x264_backend_set_bitrate;
            encoder->close = x264_backend_close;
            encoder->kind = H264_ENCODER_SW;

            log_info("encode", "software encoder: %s", selected);

            if (name_out != NULL && name_out_size > 0) {
                snprintf(name_out, name_out_size, "%s", selected);
            }

            return encoder;
        }

        log_error("encode", "libx264 failed to open");
        return NULL;
    }
#else
    if (want_sw) {
        log_error("encode", "libx264 support is not compiled in: install "
                            "libx264-dev and rebuild, or use --encoder auto on "
                            "hardware with a V4L2 M2M encoder (Raspberry Pi: "
                            "/dev/video11)");
        return NULL;
    }
#endif

    log_error("encode", "no usable backend for preference '%s'", preference);

    return NULL;
}

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
                        uint64_t *out_pts_us)
{
    if (encoder == NULL || encoder->encode == NULL) {
        return -1;
    }

    *out_size = 0;
    *out_is_idr = 0;
    *out_pts_us = pts_us;

    return encoder->encode(encoder->backend,
                           plane_y, plane_u, plane_v,
                           pts_us, force_idr,
                           out, out_capacity,
                           out_size, out_is_idr, out_pts_us);
}

H264EncoderKind h264_encoder_kind(const H264Encoder *encoder)
{
    return encoder != NULL ? encoder->kind : H264_ENCODER_SW;
}

int h264_encoder_set_bitrate(H264Encoder *encoder, uint32_t bitrate_kbps)
{
    if (encoder == NULL || encoder->set_bitrate == NULL) {
        return -1;
    }

    return encoder->set_bitrate(encoder->backend, bitrate_kbps);
}

void h264_encoder_close(H264Encoder *encoder)
{
    if (encoder == NULL) {
        return;
    }

    if (encoder->close != NULL && encoder->backend != NULL) {
        encoder->close(encoder->backend);
    }

    free(encoder);
}
