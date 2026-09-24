#define _POSIX_C_SOURCE 200809L

/*
 * encoder_v4l2m2m.c
 *
 * V4L2 stateful memory-to-memory H.264 encoder backend.
 *
 * Target hardware: the Raspberry Pi bcm2835-codec encoder (/dev/video11
 * on a Pi 1 to 4; a Pi 5 has no H.264 encoder), plus any other V4L2 M2M
 * encoder that accepts single plane YU12 or NV12.
 *
 * Queue naming is inverted for encoders:
 *   OUTPUT  queue  receives raw frames (YU12 preferred: our frames are
 *                  already I420, so it is a plain row copy)
 *   CAPTURE queue  produces Annex-B H.264
 *
 * Setup order (the one rpicam-apps uses with this driver):
 *   controls -> S_FMT OUTPUT -> S_FMT CAPTURE -> S_PARM -> REQBUFS/mmap
 *   -> queue every CAPTURE buffer -> STREAMON OUTPUT -> STREAMON CAPTURE
 *
 * Why VIDIOC_STREAMON (capture) used to fail with errno 11: the CAPTURE
 * format was set with width = height = 0. For the encoder role the
 * bcm2835 driver neither copies the OUTPUT size to the CAPTURE side nor
 * clamps a compressed format's size, so the firmware's H.264 port was
 * enabled as 0x0. The firmware refuses that port configuration, and
 * the driver hands the firmware status back as a negative errno; the
 * "not configured" status has the value 11, which reads as EAGAIN.
 * Retrying could never help. The CAPTURE format now carries the real
 * picture size.
 *
 * Other properties of this driver that the code relies on:
 *   - bytesperline of the raw side is aligned by the driver (32 bytes
 *     for YU12), so the copy honours the returned stride;
 *   - a keyframe is forced through V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME,
 *     not through a flag on the input buffer;
 *   - with REPEAT_SEQ_HEADER the SPS/PPS may arrive as a buffer of their
 *     own ahead of the IDR, so a header-only buffer is merged with the
 *     picture that follows it;
 *   - controls are set one by one: older kernels lack some of them, and
 *     one unknown control makes a batched VIDIOC_S_EXT_CTRLS fail as a
 *     whole, which silently left bitrate and GOP at their defaults.
 */
#include "h264_encoder.h"

#include "h264_bitstream.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define M2M_OUTPUT_BUFFERS 4
#define M2M_CAPTURE_BUFFERS 4
#define M2M_CAPTURE_SIZE (512 * 1024)
#define M2M_STREAMON_ATTEMPTS 3
#define M2M_MAX_CONSECUTIVE_ERRORS 30

struct M2mBuffer {
    void *start;
    size_t length;
};

struct M2mBackend {
    int fd;
    uint32_t width;
    uint32_t height;
    uint32_t fps;

    uint32_t input_format;      /* V4L2_PIX_FMT_YUV420 or NV12 */
    uint32_t bytesperline;      /* luma stride of an input buffer */
    uint32_t plane_height;      /* rows of the luma plane in the buffer */
    uint32_t sizeimage;         /* bytes of one input buffer */

    int streaming;
    int have_force_key;         /* FORCE_KEY_FRAME control available */
    int pending_idr;            /* keyframe asked while input was full */

    struct M2mBuffer out_bufs[M2M_OUTPUT_BUFFERS];
    unsigned out_count;
    unsigned out_free[M2M_OUTPUT_BUFFERS];
    unsigned out_free_count;

    struct M2mBuffer cap_bufs[M2M_CAPTURE_BUFFERS];
    unsigned cap_count;

    unsigned errors;            /* consecutive failures */
    unsigned stall_logged;
    unsigned overflow_logged;

    char name[96];
};

static int xioctl(int fd, unsigned long request, void *arg)
{
    int result;

    do {
        result = ioctl(fd, request, arg);
    } while (result == -1 && errno == EINTR);

    return result;
}

static void sleep_ms(unsigned ms)
{
    struct timespec delay = { (time_t) (ms / 1000), (long) (ms % 1000) * 1000000L };

    while (nanosleep(&delay, &delay) == -1 && errno == EINTR) {
    }
}

/*
 * Probe a device: is it a V4L2 M2M encoder that outputs H.264?
 * Returns 1 when usable.
 */
static int m2m_probe_device(const char *path)
{
    int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
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
        /* The CAPTURE queue (encoded side of an encoder) offers H.264? */
        struct v4l2_fmtdesc fmt;
        memset(&fmt, 0, sizeof(fmt));
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

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

/*
 * Set one control. 'important' ones are reported as warnings because
 * the stream then deviates from what the SDP and the configuration
 * promise; the rest only at debug level.
 */
static int m2m_set_ctrl(int fd, uint32_t id, int32_t value,
                        const char *what, int important)
{
    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = id;
    control.value = value;

    if (xioctl(fd, VIDIOC_S_CTRL, &control) == 0) {
        return 0;
    }

    if (important) {
        log_warn("encode", "m2m: control '%s' = %d rejected: errno=%d (%s)",
                 what, (int) value, errno, strerror(errno));
    } else {
        log_debug("encode", "m2m: optional control '%s' = %d rejected: "
                            "errno=%d (%s)", what, (int) value, errno,
                  strerror(errno));
    }

    return -1;
}

static int m2m_has_ctrl(int fd, uint32_t id)
{
    struct v4l2_queryctrl query;

    memset(&query, 0, sizeof(query));
    query.id = id;

    return xioctl(fd, VIDIOC_QUERYCTRL, &query) == 0 &&
           !(query.flags & V4L2_CTRL_FLAG_DISABLED);
}

static void m2m_set_controls(struct M2mBackend *encoder,
                             uint32_t bitrate_kbps,
                             uint32_t gop_frames)
{
    int fd = encoder->fd;

    m2m_set_ctrl(fd, V4L2_CID_MPEG_VIDEO_BITRATE_MODE,
                 V4L2_MPEG_VIDEO_BITRATE_MODE_CBR, "bitrate mode CBR", 0);
    m2m_set_ctrl(fd, V4L2_CID_MPEG_VIDEO_BITRATE,
                 (int32_t) (bitrate_kbps * 1000), "bitrate", 1);
    m2m_set_ctrl(fd, V4L2_CID_MPEG_VIDEO_H264_PROFILE,
                 V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE,
                 "profile constrained baseline", 1);

    /* 3.1 covers 1280x720 at 30 fps; anything larger needs 4.0. */
    int32_t level = ((uint64_t) encoder->width * encoder->height <= 1280u * 720u &&
                     encoder->fps <= 30)
                        ? V4L2_MPEG_VIDEO_H264_LEVEL_3_1
                        : V4L2_MPEG_VIDEO_H264_LEVEL_4_0;

    m2m_set_ctrl(fd, V4L2_CID_MPEG_VIDEO_H264_LEVEL, level, "level", 0);

    if (m2m_set_ctrl(fd, V4L2_CID_MPEG_VIDEO_H264_I_PERIOD,
                     (int32_t) gop_frames, "H.264 I period", 0) != 0) {
        m2m_set_ctrl(fd, V4L2_CID_MPEG_VIDEO_GOP_SIZE, (int32_t) gop_frames,
                     "GOP size", 1);
    }

    /* SPS/PPS with every IDR, so a viewer can join at any keyframe. */
    m2m_set_ctrl(fd, V4L2_CID_MPEG_VIDEO_REPEAT_SEQ_HEADER, 1,
                 "repeat sequence header", 1);

    encoder->have_force_key = m2m_has_ctrl(fd, V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME);
}

/*
 * Live bitrate change. Drivers that do not expose a writable bitrate
 * control simply reject the ioctl and the caller reports the setting
 * as requiring a restart.
 */
static int m2m_set_bitrate(struct M2mBackend *encoder, uint32_t bitrate_kbps)
{
    if (encoder == NULL || encoder->fd < 0 || bitrate_kbps == 0) {
        return -1;
    }

    return m2m_set_ctrl(encoder->fd, V4L2_CID_MPEG_VIDEO_BITRATE,
                        (int32_t) (bitrate_kbps * 1000), "bitrate", 1);
}

static int m2m_set_output_format(struct M2mBackend *encoder, const char *path)
{
    static const uint32_t input_formats[] = {
        V4L2_PIX_FMT_YUV420,        /* I420: straight row copies */
        V4L2_PIX_FMT_NV12
    };

    for (size_t i = 0; i < sizeof(input_formats) / sizeof(input_formats[0]); i++) {
        struct v4l2_format format;

        memset(&format, 0, sizeof(format));
        format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        format.fmt.pix_mp.width = encoder->width;
        format.fmt.pix_mp.height = encoder->height;
        format.fmt.pix_mp.pixelformat = input_formats[i];
        format.fmt.pix_mp.field = V4L2_FIELD_NONE;
        format.fmt.pix_mp.num_planes = 1;

        if (xioctl(encoder->fd, VIDIOC_S_FMT, &format) == -1) {
            log_debug("encode", "m2m: %s: S_FMT output %.4s: errno=%d (%s)",
                      path, (const char *) &input_formats[i], errno,
                      strerror(errno));
            continue;
        }

        const struct v4l2_pix_format_mplane *pix = &format.fmt.pix_mp;
        uint32_t bpl = pix->plane_fmt[0].bytesperline;
        uint32_t rows = pix->height;

        if (pix->pixelformat != input_formats[i] || pix->num_planes != 1 ||
            pix->width != encoder->width || rows < encoder->height ||
            bpl < encoder->width) {
            log_debug("encode", "m2m: %s: output format %.4s adjusted to "
                                "%ux%u stride %u planes %u; not usable",
                      path, (const char *) &input_formats[i], pix->width,
                      rows, bpl, pix->num_planes);
            continue;
        }

        /* Luma, then chroma (two half planes, or one interleaved). */
        size_t needed = (size_t) bpl * rows + (size_t) bpl * (rows / 2);

        if (pix->plane_fmt[0].sizeimage < needed) {
            log_debug("encode", "m2m: %s: sizeimage %u below %zu", path,
                      pix->plane_fmt[0].sizeimage, needed);
            continue;
        }

        encoder->input_format = input_formats[i];
        encoder->bytesperline = bpl;
        encoder->plane_height = rows;
        encoder->sizeimage = pix->plane_fmt[0].sizeimage;
        return 0;
    }

    log_error("encode", "m2m: %s accepts neither YU12 nor NV12 input at "
                        "%ux%u", path, encoder->width, encoder->height);
    return -1;
}

static int m2m_set_capture_format(struct M2mBackend *encoder, const char *path)
{
    struct v4l2_format format;

    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    format.fmt.pix_mp.width = encoder->width;       /* never 0: see top */
    format.fmt.pix_mp.height = encoder->height;
    format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
    format.fmt.pix_mp.field = V4L2_FIELD_NONE;
    format.fmt.pix_mp.num_planes = 1;
    format.fmt.pix_mp.plane_fmt[0].sizeimage = M2M_CAPTURE_SIZE;

    if (xioctl(encoder->fd, VIDIOC_S_FMT, &format) == -1) {
        log_error("encode", "m2m: %s: S_FMT capture H.264 %ux%u: errno=%d (%s)",
                  path, encoder->width, encoder->height, errno,
                  strerror(errno));
        return -1;
    }

    if (format.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_H264) {
        log_error("encode", "m2m: %s: capture queue refused H.264", path);
        return -1;
    }

    return 0;
}

static int m2m_map_buffers(struct M2mBackend *encoder, uint32_t type,
                           unsigned wanted, struct M2mBuffer *bufs,
                           unsigned *count_out, const char *what)
{
    struct v4l2_requestbuffers request;

    memset(&request, 0, sizeof(request));
    request.count = wanted;
    request.type = type;
    request.memory = V4L2_MEMORY_MMAP;

    if (xioctl(encoder->fd, VIDIOC_REQBUFS, &request) == -1 ||
        request.count < 1) {
        log_error("encode", "m2m: REQBUFS (%s): errno=%d (%s)", what, errno,
                  strerror(errno));
        return -1;
    }

    unsigned count = request.count < wanted ? request.count : wanted;

    for (unsigned i = 0; i < count; i++) {
        struct v4l2_buffer buffer;
        struct v4l2_plane planes[VIDEO_MAX_PLANES];

        memset(&buffer, 0, sizeof(buffer));
        memset(planes, 0, sizeof(planes));
        buffer.type = type;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = i;
        buffer.length = 1;
        buffer.m.planes = planes;

        if (xioctl(encoder->fd, VIDIOC_QUERYBUF, &buffer) == -1) {
            log_error("encode", "m2m: QUERYBUF (%s): errno=%d (%s)", what,
                      errno, strerror(errno));
            return -1;
        }

        void *start = mmap(NULL, planes[0].length, PROT_READ | PROT_WRITE,
                           MAP_SHARED, encoder->fd, planes[0].m.mem_offset);

        if (start == MAP_FAILED) {
            log_error("encode", "m2m: mmap (%s): errno=%d (%s)", what, errno,
                      strerror(errno));
            return -1;
        }

        bufs[i].start = start;
        bufs[i].length = planes[0].length;
        *count_out = i + 1;
    }

    return 0;
}

static int m2m_queue_capture(struct M2mBackend *encoder, unsigned index)
{
    struct v4l2_buffer buffer;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];

    memset(&buffer, 0, sizeof(buffer));
    memset(planes, 0, sizeof(planes));
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = index;
    buffer.length = 1;
    buffer.m.planes = planes;
    planes[0].length = (uint32_t) encoder->cap_bufs[index].length;

    if (xioctl(encoder->fd, VIDIOC_QBUF, &buffer) == -1) {
        log_error("encode", "m2m: QBUF (capture %u): errno=%d (%s)", index,
                  errno, strerror(errno));
        return -1;
    }

    return 0;
}

static int m2m_streamon(struct M2mBackend *encoder, uint32_t type,
                        const char *what, const char *path)
{
    for (int attempt = 1; ; attempt++) {
        enum v4l2_buf_type buf_type = (enum v4l2_buf_type) type;

        if (xioctl(encoder->fd, VIDIOC_STREAMON, &buf_type) == 0) {
            return 0;
        }

        int error = errno;

        /*
         * EBUSY can be a previous user (a camstream that just exited)
         * still releasing the firmware component, so it gets a short
         * retry. See the top of the file for what errno 11 means here.
         */
        if ((error == EBUSY || error == EAGAIN) &&
            attempt < M2M_STREAMON_ATTEMPTS) {
            sleep_ms(100);
            continue;
        }

        log_error("encode", "m2m: %s: STREAMON (%s) failed after %d "
                            "attempt(s): errno=%d (%s)%s", path, what, attempt,
                  error, strerror(error),
                  error == EAGAIN
                      ? "; the encoder firmware rejected the port "
                        "configuration (reported as errno 11)"
                      : error == EBUSY
                            ? "; the hardware encoder is in use by another "
                              "process (rpicam-vid --codec h264, another "
                              "camstream?)"
                            : "");
        return -1;
    }
}

static void m2m_close(struct M2mBackend *encoder)
{
    if (encoder == NULL) {
        return;
    }

    if (encoder->fd >= 0 && encoder->streaming) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        xioctl(encoder->fd, VIDIOC_STREAMOFF, &type);
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        xioctl(encoder->fd, VIDIOC_STREAMOFF, &type);
    }

    for (unsigned i = 0; i < M2M_OUTPUT_BUFFERS; i++) {
        if (encoder->out_bufs[i].start != NULL) {
            munmap(encoder->out_bufs[i].start, encoder->out_bufs[i].length);
        }
    }

    for (unsigned i = 0; i < M2M_CAPTURE_BUFFERS; i++) {
        if (encoder->cap_bufs[i].start != NULL) {
            munmap(encoder->cap_bufs[i].start, encoder->cap_bufs[i].length);
        }
    }

    if (encoder->fd >= 0) {
        /* Release the buffers explicitly, then the firmware component. */
        struct v4l2_requestbuffers request;

        memset(&request, 0, sizeof(request));
        request.memory = V4L2_MEMORY_MMAP;
        request.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        xioctl(encoder->fd, VIDIOC_REQBUFS, &request);
        request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        xioctl(encoder->fd, VIDIOC_REQBUFS, &request);

        close(encoder->fd);
    }

    free(encoder);
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

    encoder->width = width;
    encoder->height = height;
    encoder->fps = fps;

    encoder->fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (encoder->fd == -1) {
        log_error("encode", "m2m: cannot open %s: errno=%d (%s)", path, errno,
                  strerror(errno));
        free(encoder);
        return NULL;
    }

    uint32_t gop_frames = fps * (gop_seconds > 0 ? gop_seconds : 1);

    m2m_set_controls(encoder, bitrate_kbps, gop_frames);

    if (m2m_set_output_format(encoder, path) != 0 ||
        m2m_set_capture_format(encoder, path) != 0) {
        goto fail;
    }

    struct v4l2_streamparm parm;

    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    parm.parm.output.timeperframe.numerator = 1;
    parm.parm.output.timeperframe.denominator = fps;

    if (xioctl(encoder->fd, VIDIOC_S_PARM, &parm) == -1) {
        log_debug("encode", "m2m: S_PARM %u fps: errno=%d (%s)", fps, errno,
                  strerror(errno));
    }

    if (m2m_map_buffers(encoder, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                        M2M_OUTPUT_BUFFERS, encoder->out_bufs,
                        &encoder->out_count, "output") != 0 ||
        m2m_map_buffers(encoder, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                        M2M_CAPTURE_BUFFERS, encoder->cap_bufs,
                        &encoder->cap_count, "capture") != 0) {
        goto fail;
    }

    for (unsigned i = 0; i < encoder->out_count; i++) {
        if (encoder->out_bufs[i].length < encoder->sizeimage) {
            log_error("encode", "m2m: output buffer %u is %zu bytes, the "
                                "format needs %u", i,
                      encoder->out_bufs[i].length, encoder->sizeimage);
            goto fail;
        }
        encoder->out_free[encoder->out_free_count++] = i;
    }

    for (unsigned i = 0; i < encoder->cap_count; i++) {
        if (m2m_queue_capture(encoder, i) != 0) {
            goto fail;
        }
    }

    encoder->streaming = 1;     /* STREAMOFF on failure from here on */

    if (m2m_streamon(encoder, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, "output",
                     path) != 0 ||
        m2m_streamon(encoder, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, "capture",
                     path) != 0) {
        goto fail;
    }

    snprintf(encoder->name, sizeof(encoder->name),
             "v4l2 m2m %s %ux%u %s @ %u kbps", path, width, height,
             encoder->input_format == V4L2_PIX_FMT_NV12 ? "NV12" : "YU12",
             bitrate_kbps);

    log_info("encode", "m2m: encoder ready on %s: %ux%u @ %u fps, input %s "
                       "stride %u, GOP %u frames, forced keyframes %s", path,
             width, height, fps,
             encoder->input_format == V4L2_PIX_FMT_NV12 ? "NV12" : "YU12",
             encoder->bytesperline, gop_frames,
             encoder->have_force_key ? "supported" : "not supported");

    if (name_out != NULL && name_out_size > 0) {
        snprintf(name_out, name_out_size, "%s", encoder->name);
    }

    return encoder;

fail:
    m2m_close(encoder);
    return NULL;
}

/* Take back every input buffer the encoder has finished reading. */
static int m2m_reclaim_inputs(struct M2mBackend *encoder)
{
    for (;;) {
        struct v4l2_buffer buffer;
        struct v4l2_plane planes[VIDEO_MAX_PLANES];

        memset(&buffer, 0, sizeof(buffer));
        memset(planes, 0, sizeof(planes));
        buffer.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.length = 1;
        buffer.m.planes = planes;

        if (xioctl(encoder->fd, VIDIOC_DQBUF, &buffer) == -1) {
            if (errno == EAGAIN) {
                return 0;
            }
            log_error("encode", "m2m: DQBUF (output): errno=%d (%s)", errno,
                      strerror(errno));
            return -1;
        }

        if (buffer.index < encoder->out_count &&
            encoder->out_free_count < M2M_OUTPUT_BUFFERS) {
            encoder->out_free[encoder->out_free_count++] = buffer.index;
        }
    }
}

/* Copy an I420 frame into an input buffer, honouring the driver stride. */
static void m2m_fill_input(const struct M2mBackend *encoder, uint8_t *dst,
                           const uint8_t *plane_y, const uint8_t *plane_u,
                           const uint8_t *plane_v)
{
    const size_t width = encoder->width;
    const size_t height = encoder->height;
    const size_t stride = encoder->bytesperline;
    const size_t chroma_w = width / 2;
    const size_t chroma_h = height / 2;
    uint8_t *chroma = dst + stride * encoder->plane_height;

    if (stride == width && encoder->plane_height == height) {
        memcpy(dst, plane_y, width * height);
    } else {
        for (size_t row = 0; row < height; row++) {
            memcpy(dst + row * stride, plane_y + row * width, width);
        }
    }

    if (encoder->input_format == V4L2_PIX_FMT_YUV420) {
        const size_t chroma_stride = stride / 2;
        uint8_t *dst_u = chroma;
        uint8_t *dst_v = chroma + chroma_stride * (encoder->plane_height / 2);

        for (size_t row = 0; row < chroma_h; row++) {
            memcpy(dst_u + row * chroma_stride, plane_u + row * chroma_w, chroma_w);
            memcpy(dst_v + row * chroma_stride, plane_v + row * chroma_w, chroma_w);
        }
        return;
    }

    for (size_t row = 0; row < chroma_h; row++) {
        uint8_t *uv = chroma + row * stride;
        const uint8_t *u = plane_u + row * chroma_w;
        const uint8_t *v = plane_v + row * chroma_w;

        for (size_t x = 0; x < chroma_w; x++) {
            uv[2 * x] = u[x];
            uv[2 * x + 1] = v[x];
        }
    }
}

/*
 * Dequeue one encoded buffer if one is ready, append its payload to
 * 'out' and give the buffer back to the driver.
 * Returns 1 when a buffer was consumed, 0 when none is ready, -1 on error.
 */
static int m2m_take_capture(struct M2mBackend *encoder, uint8_t *out,
                            size_t out_capacity, size_t *out_size,
                            uint64_t *out_pts_us, int *out_is_idr)
{
    struct v4l2_buffer buffer;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];

    memset(&buffer, 0, sizeof(buffer));
    memset(planes, 0, sizeof(planes));
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.length = 1;
    buffer.m.planes = planes;

    if (xioctl(encoder->fd, VIDIOC_DQBUF, &buffer) == -1) {
        if (errno == EAGAIN) {
            return 0;
        }
        log_error("encode", "m2m: DQBUF (capture): errno=%d (%s)", errno,
                  strerror(errno));
        return -1;
    }

    if (buffer.index >= encoder->cap_count) {
        return -1;
    }

    size_t offset = planes[0].data_offset;
    size_t bytes = planes[0].bytesused > offset ? planes[0].bytesused - offset : 0;

    if (bytes > 0 && offset + bytes <= encoder->cap_bufs[buffer.index].length) {
        if (*out_size + bytes <= out_capacity) {
            memcpy(out + *out_size,
                   (const uint8_t *) encoder->cap_bufs[buffer.index].start + offset,
                   bytes);
            *out_size += bytes;

            /* The M2M core copies the input timestamp to the output. */
            *out_pts_us = (uint64_t) buffer.timestamp.tv_sec * 1000000ULL +
                          (uint64_t) buffer.timestamp.tv_usec;

            if (buffer.flags & V4L2_BUF_FLAG_KEYFRAME) {
                *out_is_idr = 1;
            }
        } else {
            encoder->pending_idr = 1;
            if (encoder->overflow_logged++ < 5) {
                log_warn("encode", "m2m: %zu byte frame exceeds the access "
                                   "unit buffer; dropped, requesting a "
                                   "keyframe", *out_size + bytes);
            }
            *out_size = 0;
        }
    }

    return m2m_queue_capture(encoder, buffer.index) == 0 ? 1 : -1;
}

static int m2m_wait(const struct M2mBackend *encoder, short events,
                    int timeout_ms)
{
    struct pollfd pfd = { .fd = encoder->fd, .events = events, .revents = 0 };

    int ready = poll(&pfd, 1, timeout_ms);

    if (ready < 0 && errno != EINTR) {
        return -1;
    }

    return ready > 0 && (pfd.revents & events) ? 1 : 0;
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
                      int *out_is_idr,
                      uint64_t *out_pts_us)
{
    if (encoder->errors >= M2M_MAX_CONSECUTIVE_ERRORS) {
        return -1;
    }

    const int frame_ms = (int) (1000 / (encoder->fps ? encoder->fps : 30));

    if (m2m_reclaim_inputs(encoder) != 0) {
        encoder->errors++;
    }

    if (encoder->out_free_count == 0 &&
        m2m_wait(encoder, POLLOUT, frame_ms) > 0) {
        m2m_reclaim_inputs(encoder);
    }

    int queued = 0;

    if (encoder->out_free_count > 0) {
        unsigned index = encoder->out_free[--encoder->out_free_count];

        m2m_fill_input(encoder, encoder->out_bufs[index].start,
                       plane_y, plane_u, plane_v);

        if ((force_idr || encoder->pending_idr) && encoder->have_force_key) {
            if (m2m_set_ctrl(encoder->fd, V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME,
                             1, "force key frame", 0) == 0) {
                encoder->pending_idr = 0;
            }
        }

        struct v4l2_buffer buffer;
        struct v4l2_plane planes[VIDEO_MAX_PLANES];

        memset(&buffer, 0, sizeof(buffer));
        memset(planes, 0, sizeof(planes));
        buffer.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;
        buffer.length = 1;
        buffer.m.planes = planes;
        buffer.field = V4L2_FIELD_NONE;
        buffer.timestamp.tv_sec = (time_t) (pts_us / 1000000ULL);
        buffer.timestamp.tv_usec = (suseconds_t) (pts_us % 1000000ULL);
        planes[0].bytesused = encoder->sizeimage;
        planes[0].length = (uint32_t) encoder->out_bufs[index].length;

        if (xioctl(encoder->fd, VIDIOC_QBUF, &buffer) == -1) {
            log_error("encode", "m2m: QBUF (output): errno=%d (%s)", errno,
                      strerror(errno));
            encoder->out_free[encoder->out_free_count++] = index;
            encoder->errors++;
        } else {
            queued = 1;
        }
    } else {
        /* The encoder holds every input buffer: drop this frame. */
        if (force_idr) {
            encoder->pending_idr = 1;
        }
        if (encoder->stall_logged++ < 5) {
            log_warn("encode", "m2m: no free input buffer after %d ms; frame "
                               "dropped", frame_ms);
        }
        encoder->errors++;
    }

    /*
     * Collect at most one picture. The encoder normally returns the
     * frame just queued within a few milliseconds; waiting up to one
     * frame period keeps output in step with input instead of running
     * a frame behind. A buffer holding only SPS/PPS is merged with the
     * picture that follows it.
     */
    for (int buffers = 0; buffers < 2; buffers++) {
        int ready = m2m_wait(encoder, POLLIN, queued ? frame_ms : 0);

        if (ready <= 0) {
            break;
        }

        int taken = m2m_take_capture(encoder, out, out_capacity, out_size,
                                     out_pts_us, out_is_idr);

        if (taken < 0) {
            encoder->errors++;
            break;
        }

        if (taken == 0 || *out_size == 0) {
            break;
        }

        H264AuInfo info;

        h264_au_scan(out, *out_size, &info);

        if (info.has_slice) {
            break;
        }
    }

    if (*out_size > 0) {
        encoder->errors = 0;
        return 1;
    }

    return encoder->errors >= M2M_MAX_CONSECUTIVE_ERRORS ? -1 : 0;
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
                       size_t *out_size, int *out_is_idr,
                       uint64_t *out_pts_us)
{
    return m2m_encode((struct M2mBackend *) backend,
                      plane_y, plane_u, plane_v, pts_us,
                      force_idr, out, out_capacity, out_size, out_is_idr,
                      out_pts_us);
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
    return m2m_probe_device(path);
}
