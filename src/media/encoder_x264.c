#define _POSIX_C_SOURCE 200809L

/*
 * encoder_x264.c
 *
 * libx264 backend. Tune: zerolatency, no lookahead, closed GOP,
 * repeated SPS/PPS headers so a viewer can join the stream at any
 * keyframe.
 *
 * The whole file is guarded by HAVE_X264 (set by the Makefile) rather
 * than excluded from the source list, so a build without libx264 has no
 * unsatisfied symbols and no second place to keep in sync.
 */
#include "h264_encoder.h"

#if HAVE_X264

#include <stdio.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <x264.h>

struct X264Backend {
    x264_t *handle;
    x264_picture_t picture;
    x264_param_t params;
    uint32_t width;
    uint32_t height;
    char name[64];
};

static struct X264Backend *x264_open(uint32_t width,
                              uint32_t height,
                              uint32_t fps,
                              uint32_t bitrate_kbps,
                              uint32_t gop_seconds,
                              char *name_out,
                              size_t name_out_size)
{
    struct X264Backend *encoder = calloc(1, sizeof(*encoder));
    if (encoder == NULL) {
        return NULL;
    }

    encoder->width = width;
    encoder->height = height;

    if (x264_param_default_preset(&encoder->params,
                                  "veryfast",
                                  "zerolatency") < 0) {
        free(encoder);
        return NULL;
    }

    x264_param_t *p = &encoder->params;

    p->i_width = (int) width;
    p->i_height = (int) height;
    p->i_fps_num = (int) fps;
    p->i_fps_den = 1;
    p->i_timebase_num = 1;
    p->i_timebase_den = 1000000;        /* microsecond timebase */

    p->i_keyint_max = (int) (fps * gop_seconds);
    p->i_keyint_min = (int) fps;
    p->b_open_gop = 0;
    p->b_repeat_headers = 1;            /* SPS/PPS before every IDR */
    p->b_intra_refresh = 0;

    p->rc.i_rc_method = X264_RC_ABR;
    p->rc.i_bitrate = (int) bitrate_kbps;
    p->rc.i_vbv_max_bitrate = (int) bitrate_kbps;
    p->rc.i_vbv_buffer_size = (int) bitrate_kbps;
    p->rc.f_vbv_buffer_init = 0.5f;

    p->i_log_level = X264_LOG_ERROR;
    p->i_threads = 1;                   /* deterministic, low latency */

    /*
     * Constrained baseline keeps the stream decodable by every
     * WebRTC stack and matches profile-level-id 42e01f that the
     * SDP answer advertises.
     */
    if (x264_param_apply_profile(p, "baseline") < 0) {
        free(encoder);
        return NULL;
    }

    encoder->handle = x264_encoder_open(p);
    if (encoder->handle == NULL) {
        free(encoder);
        return NULL;
    }

    x264_picture_init(&encoder->picture);
    encoder->picture.i_type = X264_TYPE_AUTO;
    encoder->picture.img.i_csp = X264_CSP_I420;
    encoder->picture.img.i_plane = 3;

    snprintf(encoder->name, sizeof(encoder->name),
             "libx264 %ux%u @ %u kbps",
             width, height, bitrate_kbps);

    if (name_out != NULL && name_out_size > 0) {
        snprintf(name_out, name_out_size, "%s", encoder->name);
    }

    return encoder;
}

static int x264_encode(struct X264Backend *encoder,
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
    x264_picture_t *pic = &encoder->picture;

    pic->img.plane[0] = (uint8_t *) plane_y;
    pic->img.plane[1] = (uint8_t *) plane_u;
    pic->img.plane[2] = (uint8_t *) plane_v;
    pic->img.i_stride[0] = (int) encoder->width;
    pic->img.i_stride[1] = (int) encoder->width / 2;
    pic->img.i_stride[2] = (int) encoder->width / 2;
    pic->i_pts = (int64_t) pts_us;
    pic->i_type = force_idr ? X264_TYPE_IDR : X264_TYPE_AUTO;

    x264_nal_t *nals = NULL;
    int nal_count = 0;
    x264_picture_t pic_out;

    int size = x264_encoder_encode(encoder->handle,
                                   &nals,
                                   &nal_count,
                                   pic,
                                   &pic_out);

    if (size < 0) {
        return -1;
    }

    if (size == 0 || nal_count == 0) {
        return 0;
    }

    if ((size_t) size > out_capacity) {
        return -1;
    }

    /*
     * x264 defaults to Annex-B (b_annexb=1): each NAL already
     * carries a start code and the payloads are contiguous, so
     * copy the encoder buffer as-is. Prefixing another start
     * code produced empty NALs that the packetizer skipped.
     */
    memcpy(out, nals[0].p_payload, (size_t) size);
    *out_size = (size_t) size;
    *out_is_idr = pic_out.b_keyframe != 0;

    return 1;
}

/*
 * Rate changes are the only live reconfiguration libx264 reliably
 * supports; x264_encoder_reconfig() rejects structural changes such as
 * a new resolution, which is why those are reported as restart
 * required instead.
 */
static int x264_set_bitrate(struct X264Backend *encoder, uint32_t bitrate_kbps)
{
    if (encoder == NULL || encoder->handle == NULL || bitrate_kbps == 0) {
        return -1;
    }

    encoder->params.rc.i_bitrate = (int) bitrate_kbps;
    encoder->params.rc.i_vbv_max_bitrate = (int) bitrate_kbps;
    encoder->params.rc.i_vbv_buffer_size = (int) bitrate_kbps;

    if (x264_encoder_reconfig(encoder->handle, &encoder->params) < 0) {
        return -1;
    }

    return 0;
}

static void x264_close(struct X264Backend *encoder)
{
    if (encoder == NULL) {
        return;
    }

    if (encoder->handle != NULL) {
        x264_encoder_close(encoder->handle);
    }

    free(encoder);
}

void *x264_backend_open(uint32_t width, uint32_t height,
                        uint32_t fps, uint32_t bitrate_kbps,
                        uint32_t gop_seconds,
                        char *name_out, size_t name_out_size)
{
    return x264_open(width, height, fps, bitrate_kbps,
                     gop_seconds, name_out, name_out_size);
}

int x264_backend_encode(void *backend,
                        const uint8_t *plane_y,
                        const uint8_t *plane_u,
                        const uint8_t *plane_v,
                        uint64_t pts_us, int force_idr,
                        uint8_t *out, size_t out_capacity,
                        size_t *out_size, int *out_is_idr)
{
    return x264_encode((struct X264Backend *) backend,
                       plane_y, plane_u, plane_v, pts_us,
                       force_idr, out, out_capacity, out_size, out_is_idr);
}

int x264_backend_set_bitrate(void *backend, uint32_t bitrate_kbps)
{
    return x264_set_bitrate((struct X264Backend *) backend, bitrate_kbps);
}

void x264_backend_close(void *backend)
{
    x264_close((struct X264Backend *) backend);
}

#endif /* HAVE_X264 */
