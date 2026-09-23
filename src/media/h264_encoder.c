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
                       size_t *out_size, int *out_is_idr);
void m2m_backend_close(void *backend);

/*
 * encoder_x264.c (linked only when HAVE_X264 is set)
 */
#if HAVE_X264
void *x264_backend_open(uint32_t width, uint32_t height,
                        uint32_t fps, uint32_t bitrate_kbps,
                        uint32_t gop_seconds,
                        char *name_out, size_t name_out_size);
int x264_backend_encode(void *backend,
                        const uint8_t *y, const uint8_t *u, const uint8_t *v,
                        uint64_t pts_us, int force_idr,
                        uint8_t *out, size_t out_capacity,
                        size_t *out_size, int *out_is_idr);
void x264_backend_close(void *backend);
#endif

struct H264Encoder {
    void *backend;
    int (*encode)(void *backend,
                  const uint8_t *y, const uint8_t *u, const uint8_t *v,
                  uint64_t pts_us, int force_idr,
                  uint8_t *out, size_t out_capacity,
                  size_t *out_size, int *out_is_idr);
    void (*close)(void *backend);
};

#define MAX_VIDEO_DEVICE 64

static int find_m2m_device(char *path_out, size_t path_size)
{
    char path[32];

    for (unsigned int i = 0; i <= MAX_VIDEO_DEVICE; i++) {
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
    if (preference == NULL) {
        preference = "auto";
    }

    if (width % 2 != 0 || height % 2 != 0 || fps == 0) {
        fprintf(stderr, "encoder: even dimensions and nonzero fps required\n");
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
        fprintf(stderr, "encoder: unknown preference '%s' "
                "(use auto, hw, hw:/dev/videoNN or sw)\n", preference);
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
                    fprintf(stderr,
                            "encoder: no V4L2 M2M H.264 encoder detected\n");
                } else {
                    printf("encoder: no V4L2 M2M H.264 hardware encoder detected; falling back to software (libx264)\n");
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
                encoder->close = m2m_backend_close;

                printf("encoder: using hardware backend [%s]\n", selected);

                if (name_out != NULL && name_out_size > 0) {
                    snprintf(name_out, name_out_size, "%s", selected);
                }

                return encoder;
            }

            fprintf(stderr, "encoder: hardware backend %s failed to open\n",
                    path);

            if (explicit_device != NULL || !want_sw) {
                return NULL;
            }
        }
    }

    /*
     * Software fallback.
     */
#if HAVE_X264
    if (want_sw) {
        void *backend = x264_backend_open(width, height, fps,
                                          bitrate_kbps, gop_seconds,
                                          selected, sizeof(selected));

        if (backend != NULL) {
            H264Encoder *encoder = calloc(1, sizeof(*encoder));
            if (encoder == NULL) {
                x264_backend_close(backend);
                return NULL;
            }

            encoder->backend = backend;
            encoder->encode = x264_backend_encode;
            encoder->close = x264_backend_close;

            printf("encoder: using software backend [%s]\n", selected);

            if (name_out != NULL && name_out_size > 0) {
                snprintf(name_out, name_out_size, "%s", selected);
            }

            return encoder;
        }

        fprintf(stderr, "encoder: libx264 failed to open\n");
        return NULL;
    }
#else
    if (want_sw) {
        fprintf(stderr,
            "encoder: libx264 support is not compiled in.\n"
            "Install libx264-dev (or pass X264_DIR=...) and rebuild, or\n"
            "run with --encoder auto on hardware that provides a V4L2 M2M\n"
            "H.264 encoder (Raspberry Pi: /dev/video11).\n");
        return NULL;
    }
#endif

    fprintf(stderr, "encoder: no usable backend for preference '%s'\n",
            preference);

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
                        int *out_is_idr)
{
    if (encoder == NULL || encoder->encode == NULL) {
        return -1;
    }

    return encoder->encode(encoder->backend,
                           plane_y, plane_u, plane_v,
                           pts_us, force_idr,
                           out, out_capacity,
                           out_size, out_is_idr);
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
