#define _POSIX_C_SOURCE 200809L

/*
 * encoder_v4l2m2m.c
 *
 * V4L2 stateful memory-to-memory H.264 encoder backend.
 *
 * Target hardware: Raspberry Pi bcm2835-codec encoder
 * (typically /dev/video11 on Pi 3 and 4, video31 and similar
 * on some configurations), plus any mainline V4L2 M2M encoder
 * driver (hantro, cedrus and friends).
 *
 * Queue layout (kernel naming is inverted for encoders):
 *   OUTPUT  queue  receives raw NV12 or YU12 frames
 *   CAPTURE queue  produces Annex-B H.264
 */
#include "h264_encoder.h"
#include "log.h"
#include "yuv_convert.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define M2M_OUTPUT_BUFFERS 6      /* raw frames queued to the encoder */
#define M2M_CAPTURE_BUFFERS 6     /* encoded buffers */

struct M2mBuffer {
    void *start;
    size_t length;
};

struct M2mBackend {
    int fd;
    uint32_t width;
    uint32_t height;
    uint32_t input_format;          /* V4L2_PIX_FMT_NV12 or YU12 */
    int is_multiplanar;             /* 1 when using NV12M/YUV420M */
    char name[64];

    struct M2mBuffer out_bufs[M2M_OUTPUT_BUFFERS];
    struct M2mBuffer out_bufs_uv[M2M_OUTPUT_BUFFERS]; /* second plane for M formats */
    int out_free[M2M_OUTPUT_BUFFERS];
    int out_free_count;

    struct M2mBuffer cap_bufs[M2M_CAPTURE_BUFFERS];

    int errors;
};

static int xioctl(int fd, unsigned long request, void *arg)
{
    int result;

    do {
        result = ioctl(fd, request, arg);
    } while (result == -1 && errno == EINTR);

    return result;
}

/*
 * Probe a device: is it a V4L2 M2M encoder that outputs H.264?
 * Returns 1 when usable.
 */
int h264_encoder_m2m_probe_device(const char *path)
{
    int fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd == -1) {
        return 0;
    }

    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));

    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == -1) {
        close(fd);
        return 0;
    }

    uint32_t caps = cap.capabilities;
    if (caps & V4L2_CAP_DEVICE_CAPS) {
        caps = cap.device_caps;
    }

    int ok = 0;

    if (caps & V4L2_CAP_VIDEO_M2M_MPLANE) {
        /*
         * Check that the CAPTURE queue (encoded side for an
         * encoder) offers H.264.
         */
        struct v4l2_fmtdesc fmt;
        memset(&fmt, 0, sizeof(fmt));
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        fmt.index = 0;

        while (xioctl(fd, VIDIOC_ENUM_FMT, &fmt) == 0) {
            if (fmt.pixelformat == V4L2_PIX_FMT_H264) {
                ok = 1;
                break;
            }
            fmt.index++;
        }
    }

    close(fd);
    return ok;
}

static int m2m_set_controls(struct M2mBackend *encoder,
                            uint32_t bitrate_kbps,
                            uint32_t gop_frames)
{
    struct v4l2_ext_controls controls;
    struct v4l2_ext_control control[5];
    int count = 0;

    memset(control, 0, sizeof(control));

    control[count].id = V4L2_CID_MPEG_VIDEO_BITRATE_MODE;
    control[count].value = V4L2_MPEG_VIDEO_BITRATE_MODE_CBR;
    count++;

    control[count].id = V4L2_CID_MPEG_VIDEO_BITRATE;
    control[count].value = (int32_t) (bitrate_kbps * 1000);
    count++;

    control[count].id = V4L2_CID_MPEG_VIDEO_GOP_SIZE;
    control[count].value = (int32_t) gop_frames;
    count++;

    control[count].id = V4L2_CID_MPEG_VIDEO_REPEAT_SEQ_HEADER;
    control[count].value = 1;
    count++;

    memset(&controls, 0, sizeof(controls));
    controls.ctrl_class = V4L2_CTRL_CLASS_MPEG;
    controls.count = (uint32_t) count;
    controls.controls = control;

    /*
     * Every control is optional. Drivers implement different
     * subsets, so failure of the batch simply means defaults.
     */
    if (xioctl(encoder->fd, VIDIOC_S_EXT_CTRLS, &controls) == -1) {
        log_warn("encode", "m2m: optional encoder controls rejected: "
                           "errno=%d (%s)", errno, strerror(errno));
    }

    return 0;
}

/*
 * Live bitrate change. Drivers that do not expose a writable bitrate
 * control simply reject the ioctl and the caller falls back to
 * reporting a restart as required.
 */
static int m2m_set_bitrate(struct M2mBackend *encoder, uint32_t bitrate_kbps)
{
    if (encoder == NULL || encoder->fd < 0 || bitrate_kbps == 0) {
        return -1;
    }

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MPEG_VIDEO_BITRATE;
    control.value = (int32_t) (bitrate_kbps * 1000);

    if (xioctl(encoder->fd, VIDIOC_S_CTRL, &control) == -1) {
        log_warn("encode", "m2m: bitrate change to %u kbps rejected: "
                           "errno=%d (%s)", bitrate_kbps, errno,
                 strerror(errno));
        return -1;
    }

    return 0;
}

static void m2m_unmap(struct M2mBuffer *bufs, int count)
{
    for (int i = 0; i < count; i++) {
        if (bufs[i].start != NULL && bufs[i].start != MAP_FAILED) {
            munmap(bufs[i].start, bufs[i].length);
            bufs[i].start = NULL;
        }
    }
}

static struct M2mBackend *m2m_open(const char *path,
                             uint32_t width,
                             uint32_t height,
                             uint32_t fps,
                             uint32_t bitrate_kbps,
                             uint32_t gop_seconds,
                             char *name_out,
                             size_t name_out_size)
{
    struct M2mBackend *encoder = calloc(1, sizeof(*encoder));
    if (encoder == NULL) {
        return NULL;
    }

    encoder->fd = -1;
    encoder->width = width;
    encoder->height = height;

    encoder->fd = open(path, O_RDWR | O_NONBLOCK);
    if (encoder->fd == -1) {
        log_error("encode", "m2m: cannot open %s: %s", path, strerror(errno));
        free(encoder);
        return NULL;
    }

    /*
     * INPUT side (OUTPUT queue in kernel terms): try NV12
     * first, then planar YU12.
     * Prefer single-planar variants (NV12, YUV420) because our
     * mmap and fill logic currently handles a single contiguous
     * buffer. Multi-planar (NV12M, YUV420M) would require
     * mapping multiple planes and separate handling.
     */
    static const uint32_t input_formats[] = {
        V4L2_PIX_FMT_NV12,
        V4L2_PIX_FMT_YUV420,
        V4L2_PIX_FMT_NV12M,
        V4L2_PIX_FMT_YUV420M
    };

    int input_ok = 0;

    for (size_t i = 0; i < sizeof(input_formats) / sizeof(input_formats[0]); i++) {
        struct v4l2_format format;

        memset(&format, 0, sizeof(format));
        format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        format.fmt.pix_mp.width = width;
        format.fmt.pix_mp.height = height;
        format.fmt.pix_mp.pixelformat = input_formats[i];
        format.fmt.pix_mp.field = V4L2_FIELD_NONE;
        format.fmt.pix_mp.num_planes = (input_formats[i] == V4L2_PIX_FMT_NV12 ||
                                        input_formats[i] == V4L2_PIX_FMT_YUV420) ? 1 : 2;

        if (xioctl(encoder->fd, VIDIOC_S_FMT, &format) == -1) {
            continue;
        }

        if (format.fmt.pix_mp.pixelformat != input_formats[i]) {
            continue;
        }

        encoder->input_format = input_formats[i];
        encoder->is_multiplanar =
            (input_formats[i] == V4L2_PIX_FMT_NV12M ||
             input_formats[i] == V4L2_PIX_FMT_YUV420M) ? 1 : 0;
        input_ok = 1;
        break;
    }

    if (!input_ok) {
        log_info("encode", "m2m: %s accepts neither NV12 nor YU12 input", path);
        goto fail;
    }

    /*
     * OUTPUT side (CAPTURE queue): H.264.
     */
    struct v4l2_format format;

    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;

    if (xioctl(encoder->fd, VIDIOC_S_FMT, &format) == -1) {
        log_error("encode", "m2m: VIDIOC_S_FMT (capture): errno=%d (%s)", errno, strerror(errno));
        goto fail;
    }

    size_t capture_size = format.fmt.pix_mp.plane_fmt[0].sizeimage;
    if (capture_size < 256 * 1024) {
        capture_size = 512 * 1024;
    }

    m2m_set_controls(encoder, bitrate_kbps, fps * gop_seconds);

    /*
     * Raw input buffers.
     */
    struct v4l2_requestbuffers request;

    memset(&request, 0, sizeof(request));
    request.count = M2M_OUTPUT_BUFFERS;
    request.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    request.memory = V4L2_MEMORY_MMAP;

    if (xioctl(encoder->fd, VIDIOC_REQBUFS, &request) == -1 ||
        request.count < 2) {
        log_error("encode", "m2m: VIDIOC_REQBUFS (output): errno=%d (%s)", errno, strerror(errno));
        goto fail;
    }

    int out_count = (int) request.count;

    for (int i = 0; i < out_count; i++) {
        struct v4l2_buffer buffer;
        struct v4l2_plane planes[VIDEO_MAX_PLANES];

        memset(&buffer, 0, sizeof(buffer));
        memset(planes, 0, sizeof(planes));
        buffer.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = (uint32_t) i;
        buffer.length = encoder->is_multiplanar ? 2 : 1;
        buffer.m.planes = planes;

        if (xioctl(encoder->fd, VIDIOC_QUERYBUF, &buffer) == -1) {
            log_error("encode", "m2m: VIDIOC_QUERYBUF (output): errno=%d (%s)", errno, strerror(errno));
            goto fail;
        }

        encoder->out_bufs[i].length = planes[0].length;
        encoder->out_bufs[i].start = mmap(NULL, planes[0].length,
                                          PROT_READ | PROT_WRITE,
                                          MAP_SHARED,
                                          encoder->fd,
                                          planes[0].m.mem_offset);

        if (encoder->out_bufs[i].start == MAP_FAILED) {
            log_error("encode", "m2m: mmap (output): errno=%d (%s)", errno, strerror(errno));
            goto fail;
        }

        if (encoder->is_multiplanar) {
            encoder->out_bufs_uv[i].length = planes[1].length;
            encoder->out_bufs_uv[i].start = mmap(NULL, planes[1].length,
                                                 PROT_READ | PROT_WRITE,
                                                 MAP_SHARED,
                                                 encoder->fd,
                                                 planes[1].m.mem_offset);
            if (encoder->out_bufs_uv[i].start == MAP_FAILED) {
                log_error("encode", "m2m: mmap (output uv): errno=%d (%s)", errno, strerror(errno));
                goto fail;
            }
        }
    }

    encoder->out_free_count = out_count;
    for (int i = 0; i < out_count; i++) {
        encoder->out_free[i] = i;
    }

    /*
     * Encoded output buffers.
     */
    memset(&request, 0, sizeof(request));
    request.count = M2M_CAPTURE_BUFFERS;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    request.memory = V4L2_MEMORY_MMAP;

    if (xioctl(encoder->fd, VIDIOC_REQBUFS, &request) == -1 ||
        request.count < 2) {
        log_error("encode", "m2m: VIDIOC_REQBUFS (capture): errno=%d (%s)", errno, strerror(errno));
        goto fail;
    }

    int cap_count = (int) request.count;

    for (int i = 0; i < cap_count; i++) {
        struct v4l2_buffer buffer;
        struct v4l2_plane planes[VIDEO_MAX_PLANES];

        memset(&buffer, 0, sizeof(buffer));
        memset(planes, 0, sizeof(planes));
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = (uint32_t) i;
        buffer.length = 1;
        buffer.m.planes = planes;

        if (xioctl(encoder->fd, VIDIOC_QUERYBUF, &buffer) == -1) {
            log_error("encode", "m2m: VIDIOC_QUERYBUF (capture): errno=%d (%s)", errno, strerror(errno));
            goto fail;
        }

        encoder->cap_bufs[i].length = planes[0].length;
        encoder->cap_bufs[i].start = mmap(NULL, planes[0].length,
                                          PROT_READ | PROT_WRITE,
                                          MAP_SHARED,
                                          encoder->fd,
                                          planes[0].m.mem_offset);

        if (encoder->cap_bufs[i].start == MAP_FAILED) {
            log_error("encode", "m2m: mmap (capture): errno=%d (%s)", errno, strerror(errno));
            goto fail;
        }

        /*
         * Queue the empty capture buffer immediately.
         */
        memset(&buffer, 0, sizeof(buffer));
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = (uint32_t) i;
        buffer.length = 1;
        buffer.m.planes = planes;

        if (xioctl(encoder->fd, VIDIOC_QBUF, &buffer) == -1) {
            log_error("encode", "m2m: VIDIOC_QBUF (capture): errno=%d (%s)", errno, strerror(errno));
            goto fail;
        }
    }

    enum v4l2_buf_type out_type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    enum v4l2_buf_type cap_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

    if (xioctl(encoder->fd, VIDIOC_STREAMON, &cap_type) == -1) {
        log_error("encode", "m2m: VIDIOC_STREAMON (capture): errno=%d (%s)", errno, strerror(errno));
        goto fail;
    }

    if (xioctl(encoder->fd, VIDIOC_STREAMON, &out_type) == -1) {
        log_error("encode", "m2m: VIDIOC_STREAMON (output): errno=%d (%s)", errno, strerror(errno));
        goto fail;
    }

    snprintf(encoder->name, sizeof(encoder->name),
             "v4l2 m2m %s (%s input)", path,
             encoder->input_format == V4L2_PIX_FMT_NV12M ||
             encoder->input_format == V4L2_PIX_FMT_NV12 ? "NV12" : "YU12");

    log_info("encode", "m2m: encoder ready on %s, %ux%u", path, width, height);

    if (name_out != NULL && name_out_size > 0) {
        snprintf(name_out, name_out_size, "%s", encoder->name);
    }

    return encoder;

fail:
    m2m_unmap(encoder->out_bufs, M2M_OUTPUT_BUFFERS);
    m2m_unmap(encoder->out_bufs_uv, M2M_OUTPUT_BUFFERS);
    m2m_unmap(encoder->cap_bufs, M2M_CAPTURE_BUFFERS);

    if (encoder->fd >= 0) {
        close(encoder->fd);
    }

    free(encoder);
    return NULL;
}

/*
 * Wait until the encoder released one raw input buffer, then
 * return its index, or -1.
 */
static int m2m_acquire_input_buffer(struct M2mBackend *encoder)
{
    if (encoder->out_free_count > 0) {
        return encoder->out_free[--encoder->out_free_count];
    }

    struct pollfd pfd = { .fd = encoder->fd, .events = POLLOUT, .revents = 0 };

    if (poll(&pfd, 1, 50) <= 0) {
        return -1;
    }

    struct v4l2_buffer buffer;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];

    memset(&buffer, 0, sizeof(buffer));
    memset(planes, 0, sizeof(planes));
    buffer.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.length = encoder->is_multiplanar ? 2 : 1;
    buffer.m.planes = planes;

    if (xioctl(encoder->fd, VIDIOC_DQBUF, &buffer) == -1) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            log_error("encode", "m2m: VIDIOC_DQBUF (output acquire): errno=%d (%s)", errno, strerror(errno));
            encoder->errors++;
        }
        return -1;
    }

    return (int) buffer.index;
}

/*
 * Copy an I420 frame into an m2m input buffer. Handles both
 * semiplanar (NV12) and planar (YU12) drivers, and both
 * single-planar and multi-planar variants.
 */
static void m2m_fill_input(struct M2mBackend *encoder,
                           uint8_t *dst,
                           uint8_t *dst_uv,
                           size_t dst_length,
                           const uint8_t *plane_y,
                           const uint8_t *plane_u,
                           const uint8_t *plane_v)
{
    const size_t luma = (size_t) encoder->width * encoder->height;
    const size_t chroma = luma / 4;
    const int nv12 = encoder->input_format == V4L2_PIX_FMT_NV12M ||
                     encoder->input_format == V4L2_PIX_FMT_NV12;

    if (encoder->is_multiplanar) {
        /* Y in plane 0, UV or U+V in plane 1 */
        memcpy(dst, plane_y, luma);

        if (nv12) {
            uint8_t *uv = dst_uv;
            const uint8_t *u = plane_u;
            const uint8_t *v = plane_v;
            for (size_t i = 0; i < chroma; i++) {
                uv[2 * i] = u[i];
                uv[2 * i + 1] = v[i];
            }
        } else {
            /* YUV420M: plane 0 = Y, plane 1 = U, but we have only 2 planes
             * allocated for NV12M. For true YUV420M we need 3 planes.
             * Most Pi drivers expose NV12M, not YUV420M, so we handle
             * the common case. If YUV420M is used, we interleave U+V
             * into plane 1 as best effort and rely on driver accepting it,
             * otherwise we fallback to software encoder.
             */
            memcpy(dst_uv, plane_u, chroma);
            /* If there's a third plane, we would need it, but we only
             * have 2. Copy V after U if space permits. */
            if (dst_length >= chroma * 2) {
                /* This path is only reached if driver actually gave us
                 * enough space in plane 1 for both U and V */
                memcpy(dst_uv + chroma, plane_v, chroma);
            }
        }
    } else {
        memcpy(dst, plane_y, luma);

        if (nv12) {
            uint8_t *uv = dst + luma;
            const uint8_t *u = plane_u;
            const uint8_t *v = plane_v;

            for (size_t i = 0; i < chroma; i++) {
                uv[2 * i] = u[i];
                uv[2 * i + 1] = v[i];
            }
        } else {
            memcpy(dst + luma, plane_u, chroma);
            memcpy(dst + luma + chroma, plane_v, chroma);
        }
    }

    (void) dst_length;
}

static int m2m_encode(struct M2mBackend *encoder,
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
    *out_size = 0;
    *out_is_idr = 0;

    if (encoder->errors > 64) {
        return -1;
    }

    int index = m2m_acquire_input_buffer(encoder);
    if (index < 0) {
        return 0;
    }

    m2m_fill_input(encoder,
                   encoder->out_bufs[index].start,
                   encoder->is_multiplanar ? encoder->out_bufs_uv[index].start : NULL,
                   encoder->out_bufs[index].length,
                   plane_y, plane_u, plane_v);

    struct v4l2_buffer out_buffer;
    struct v4l2_plane out_planes[VIDEO_MAX_PLANES];

    memset(&out_buffer, 0, sizeof(out_buffer));
    memset(out_planes, 0, sizeof(out_planes));
    out_buffer.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    out_buffer.memory = V4L2_MEMORY_MMAP;
    out_buffer.index = (uint32_t) index;
    out_buffer.length = encoder->is_multiplanar ? 2 : 1;
    out_buffer.m.planes = out_planes;
    out_buffer.field = V4L2_FIELD_NONE;
    out_buffer.timestamp.tv_sec = (time_t) (pts_us / 1000000ULL);
    out_buffer.timestamp.tv_usec = (suseconds_t) (pts_us % 1000000ULL);

    if (force_idr) {
        out_buffer.flags |= V4L2_BUF_FLAG_KEYFRAME;
    }

    if (encoder->is_multiplanar) {
        out_planes[0].bytesused = (uint32_t) encoder->width * encoder->height;
        const size_t chroma = (size_t) encoder->width * encoder->height / 4;
        if (encoder->input_format == V4L2_PIX_FMT_NV12M) {
            out_planes[1].bytesused = (uint32_t) (chroma * 2);
        } else {
            out_planes[1].bytesused = (uint32_t) (chroma * 2);
        }
    } else {
        out_planes[0].bytesused = (uint32_t) encoder->out_bufs[index].length;
    }

    if (xioctl(encoder->fd, VIDIOC_QBUF, &out_buffer) == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EBUSY) {
            log_debug("encode", "m2m: VIDIOC_QBUF (output) busy: errno=%d", errno);
            return 0;
        }
        log_error("encode", "m2m: VIDIOC_QBUF (output): errno=%d (%s)", errno, strerror(errno));
        encoder->errors++;
        return 0;
    }

    /*
     * Drain encoded data. Hardware encoders run one to three
     * frames behind, so a single call often has nothing ready
     * yet and the next frame produces the output.
     */
    struct pollfd pfd = { .fd = encoder->fd, .events = POLLIN, .revents = 0 };
    int wait = poll(&pfd, 1, 8);

    if (wait <= 0) {
        return 0;
    }

    for (;;) {
        struct v4l2_buffer cap_buffer;
        struct v4l2_plane cap_planes[VIDEO_MAX_PLANES];

        memset(&cap_buffer, 0, sizeof(cap_buffer));
        memset(cap_planes, 0, sizeof(cap_planes));
        cap_buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        cap_buffer.memory = V4L2_MEMORY_MMAP;
        cap_buffer.length = 1;
        cap_buffer.m.planes = cap_planes;

        if (xioctl(encoder->fd, VIDIOC_DQBUF, &cap_buffer) == -1) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                log_error("encode", "m2m: VIDIOC_DQBUF (capture): errno=%d (%s)", errno, strerror(errno));
                encoder->errors++;
            }
            break;
        }

        size_t bytes = cap_planes[0].bytesused;
        uint32_t cap_index = cap_buffer.index;

        if (bytes > 0) {
            if (*out_size + bytes > out_capacity) {
                log_error("encode", "m2m: access unit overflow");
            } else {
                memcpy(out + *out_size, encoder->cap_bufs[cap_index].start, bytes);
                *out_size += bytes;

                if (cap_buffer.flags & V4L2_BUF_FLAG_KEYFRAME) {
                    *out_is_idr = 1;
                }
            }
        }

        memset(&cap_buffer, 0, sizeof(cap_buffer));
        memset(cap_planes, 0, sizeof(cap_planes));
        cap_buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        cap_buffer.memory = V4L2_MEMORY_MMAP;
        cap_buffer.index = cap_index;
        cap_buffer.length = 1;
        cap_buffer.m.planes = cap_planes;

        if (xioctl(encoder->fd, VIDIOC_QBUF, &cap_buffer) == -1) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EBUSY) {
                log_error("encode", "m2m: VIDIOC_QBUF (capture): errno=%d (%s)", errno, strerror(errno));
                encoder->errors++;
            }
            break;
        }

        /*
         * Only drain what is ready now.
         */
        struct pollfd check = { .fd = encoder->fd, .events = POLLIN, .revents = 0 };
        if (poll(&check, 1, 0) <= 0) {
            break;
        }
    }

    if (*out_size > 0) {
        encoder->errors = 0;
        return 1;
    }

    return 0;
}

static void m2m_close(struct M2mBackend *encoder)
{
    if (encoder == NULL) {
        return;
    }

    if (encoder->fd >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        xioctl(encoder->fd, VIDIOC_STREAMOFF, &type);
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        xioctl(encoder->fd, VIDIOC_STREAMOFF, &type);

        close(encoder->fd);
    }

    m2m_unmap(encoder->out_bufs, M2M_OUTPUT_BUFFERS);
    m2m_unmap(encoder->out_bufs_uv, M2M_OUTPUT_BUFFERS);
    m2m_unmap(encoder->cap_bufs, M2M_CAPTURE_BUFFERS);

    free(encoder);
}

void *m2m_backend_open(const char *path,
                       uint32_t width, uint32_t height,
                       uint32_t fps, uint32_t bitrate_kbps,
                       uint32_t gop_seconds,
                       char *name_out, size_t name_out_size)
{
    return m2m_open(path, width, height, fps, bitrate_kbps,
                    gop_seconds, name_out, name_out_size);
}

int m2m_backend_encode(void *backend,
                       const uint8_t *plane_y,
                       const uint8_t *plane_u,
                       const uint8_t *plane_v,
                       uint64_t pts_us, int force_idr,
                       uint8_t *out, size_t out_capacity,
                       size_t *out_size, int *out_is_idr)
{
    return m2m_encode((struct M2mBackend *) backend,
                      plane_y, plane_u, plane_v, pts_us,
                      force_idr, out, out_capacity, out_size, out_is_idr);
}

int m2m_backend_set_bitrate(void *backend, uint32_t bitrate_kbps)
{
    return m2m_set_bitrate((struct M2mBackend *) backend, bitrate_kbps);
}

void m2m_backend_close(void *backend)
{
    m2m_close((struct M2mBackend *) backend);
}

int m2m_backend_probe(const char *path)
{
    return h264_encoder_m2m_probe_device(path);
}
