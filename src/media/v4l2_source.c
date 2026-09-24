#define _POSIX_C_SOURCE 200809L

/*
 * v4l2_source.c
 *
 * V4L2 mmap capture behind the VideoSource interface.
 *
 * Design notes:
 *   - poll() instead of select()
 *   - short poll timeout so shutdown is never delayed
 *   - YUYV preferred, YU12 (planar 4:2:0) accepted as fallback
 *   - no printf per buffer, only configuration summaries
 */
#include "video_source.h"

#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define V4L2_BUFFER_COUNT 4

struct V4l2Source {
    int fd;
    int streaming;
    struct {
        void *start;
        size_t length;
    } buffers[V4L2_BUFFER_COUNT];
    unsigned int buffer_count;

    char name[64];
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t stride;
    uint32_t format;
    size_t frame_size;
    uint64_t sequence;
};

static int xioctl(int fd, unsigned long request, void *arg)
{
    int result;

    do {
        result = ioctl(fd, request, arg);
    } while (result == -1 && errno == EINTR);

    return result;
}

static uint64_t monotonic_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t) ts.tv_sec * 1000000ULL +
           (uint64_t) ts.tv_nsec / 1000ULL;
}

static int v4l2_start(VideoSource *source)
{
    struct V4l2Source *impl = source->impl;

    if (impl->streaming) {
        return 0;
    }

    for (unsigned int i = 0; i < impl->buffer_count; i++) {
        struct v4l2_buffer buffer;

        memset(&buffer, 0, sizeof(buffer));
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = i;

        if (xioctl(impl->fd, VIDIOC_QBUF, &buffer) == -1) {
            log_error("capture", "v4l2 VIDIOC_QBUF: errno=%d (%s)", errno, strerror(errno));
            return -1;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (xioctl(impl->fd, VIDIOC_STREAMON, &type) == -1) {
        if (errno == EINVAL) {
            log_error("capture", "v4l2 VIDIOC_STREAMON failed with EINVAL (errno 22) on %s. "
                      "If this is a Raspberry Pi CSI camera (e.g., IMX219), raw V4L2 pad formats "
                      "do not match sensor resolution (3280x2464). Use '-s csi' to stream via rpicam-vid.",
                      impl->name);
        } else {
            log_error("capture", "v4l2 VIDIOC_STREAMON: errno=%d (%s)", errno, strerror(errno));
        }
        return -1;
    }

    impl->streaming = 1;

    return 0;
}

static int v4l2_capture(VideoSource *source,
                        uint64_t *out_timestamp_us,
                        const uint8_t **out_data,
                        size_t *out_size,
                        uint32_t *out_buffer_index)
{
    struct V4l2Source *impl = source->impl;

    struct pollfd pfd = {
        .fd = impl->fd,
        .events = POLLIN,
        .revents = 0
    };

    int ready = poll(&pfd, 1, 200);

    if (ready < 0) {
        if (errno == EINTR) {
            return 0;
        }
        log_error("capture", "v4l2 poll: errno=%d (%s)", errno, strerror(errno));
        return -1;
    }

    if (ready == 0) {
        return 0;
    }

    struct v4l2_buffer buffer;

    memset(&buffer, 0, sizeof(buffer));
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;

    if (xioctl(impl->fd, VIDIOC_DQBUF, &buffer) == -1) {
        if (errno == EAGAIN) {
            return 0;
        }
        log_error("capture", "v4l2 VIDIOC_DQBUF: errno=%d (%s)", errno, strerror(errno));
        return -1;
    }

    if (buffer.index >= impl->buffer_count) {
        return -1;
    }

    if (buffer.flags & V4L2_BUF_FLAG_ERROR) {
        /* Driver flagged this buffer as erroneous, recycle it */
        struct v4l2_buffer qbuf;
        memset(&qbuf, 0, sizeof(qbuf));
        qbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        qbuf.memory = V4L2_MEMORY_MMAP;
        qbuf.index = buffer.index;
        xioctl(impl->fd, VIDIOC_QBUF, &qbuf);
        return 0;
    }

    if (buffer.bytesused == 0 || buffer.bytesused > impl->frame_size) {
        /* Some drivers return 0 bytesused for empty buffers, skip */
        struct v4l2_buffer qbuf;
        memset(&qbuf, 0, sizeof(qbuf));
        qbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        qbuf.memory = V4L2_MEMORY_MMAP;
        qbuf.index = buffer.index;
        xioctl(impl->fd, VIDIOC_QBUF, &qbuf);
        return 0;
    }

    *out_timestamp_us = monotonic_us();
    *out_data = impl->buffers[buffer.index].start;
    *out_size = buffer.bytesused;
    *out_buffer_index = buffer.index;

    return 1;
}

static void v4l2_release(VideoSource *source, uint32_t buffer_index)
{
    struct V4l2Source *impl = source->impl;

    if (buffer_index >= impl->buffer_count) {
        log_info("capture", "v4l2: release with invalid index %u", buffer_index);
        return;
    }

    struct v4l2_buffer buffer;

    memset(&buffer, 0, sizeof(buffer));
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = buffer_index;

    if (xioctl(impl->fd, VIDIOC_QBUF, &buffer) == -1) {
        log_error("capture", "v4l2 VIDIOC_QBUF (release): errno=%d (%s)", errno, strerror(errno));
    }
}

static void v4l2_close(VideoSource *source)
{
    if (source == NULL) {
        return;
    }

    struct V4l2Source *impl = source->impl;

    if (impl == NULL) {
        free(source);
        return;
    }

    if (impl->streaming) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(impl->fd, VIDIOC_STREAMOFF, &type);
    }

    for (unsigned int i = 0; i < impl->buffer_count; i++) {
        if (impl->buffers[i].start != NULL &&
            impl->buffers[i].start != MAP_FAILED) {
            munmap(impl->buffers[i].start, impl->buffers[i].length);
        }
    }

    if (impl->fd >= 0) {
        close(impl->fd);
    }

    free(impl);
    free(source);
}

VideoSource *v4l2_source_create(const char *device,
                                uint32_t width,
                                uint32_t height,
                                uint32_t fps)
{
    if (device == NULL || fps == 0) {
        return NULL;
    }

    VideoSource *source = calloc(1, sizeof(*source));
    struct V4l2Source *impl = calloc(1, sizeof(*impl));

    if (source == NULL || impl == NULL) {
        free(source);
        free(impl);
        return NULL;
    }

    impl->fd = -1;
    snprintf(impl->name, sizeof(impl->name), "v4l2 %s", device);

    impl->fd = open(device, O_RDWR | O_NONBLOCK);

    if (impl->fd == -1) {
        log_error("capture", "v4l2: cannot open %s: %s", device, strerror(errno));
        free(impl);
        free(source);
        return NULL;
    }

    struct v4l2_capability capability;

    memset(&capability, 0, sizeof(capability));

    if (xioctl(impl->fd, VIDIOC_QUERYCAP, &capability) == -1) {
        log_error("capture", "v4l2 VIDIOC_QUERYCAP: errno=%d (%s)", errno, strerror(errno));
        goto fail;
    }

    uint32_t caps = capability.capabilities;
    if (caps & V4L2_CAP_DEVICE_CAPS) {
        caps = capability.device_caps;
    }

    if (!(caps & V4L2_CAP_VIDEO_CAPTURE) ||
        !(caps & V4L2_CAP_STREAMING)) {
        log_info("capture", "v4l2: %s does not support mmap capture", device);
        goto fail;
    }

    log_info("capture", "v4l2: device %s (%s)", device, capability.card);

    /*
     * Format negotiation: try YUYV first, then planar YU12.
     */
    static const uint32_t formats[] = {
        V4L2_PIX_FMT_YUYV,
        V4L2_PIX_FMT_YUV420
    };

    int format_ok = 0;

    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        struct v4l2_format format;

        memset(&format, 0, sizeof(format));
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        format.fmt.pix.width = width;
        format.fmt.pix.height = height;
        format.fmt.pix.pixelformat = formats[i];
        format.fmt.pix.field = V4L2_FIELD_NONE;

        if (xioctl(impl->fd, VIDIOC_S_FMT, &format) == -1) {
            continue;
        }

        if (format.fmt.pix.pixelformat != formats[i]) {
            continue;
        }

        impl->width = format.fmt.pix.width;
        impl->height = format.fmt.pix.height;
        impl->stride = format.fmt.pix.bytesperline;
        impl->frame_size = format.fmt.pix.sizeimage;
        impl->format = formats[i];
        format_ok = 1;
        break;
    }

    if (!format_ok) {
        log_info("capture", "v4l2: %s supports neither YUYV nor YU12", device);
        goto fail;
    }

    /*
     * Frame interval.
     */
    struct v4l2_streamparm parm;

    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = fps;

    if (xioctl(impl->fd, VIDIOC_S_PARM, &parm) == 0 &&
        parm.parm.capture.timeperframe.numerator != 0) {
        impl->fps = parm.parm.capture.timeperframe.denominator /
                    parm.parm.capture.timeperframe.numerator;
    } else {
        impl->fps = fps;
    }

    /*
     * Buffers.
     */
    struct v4l2_requestbuffers request;

    memset(&request, 0, sizeof(request));
    request.count = V4L2_BUFFER_COUNT;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;

    if (xioctl(impl->fd, VIDIOC_REQBUFS, &request) == -1) {
        log_error("capture", "v4l2 VIDIOC_REQBUFS: errno=%d (%s)", errno, strerror(errno));
        goto fail;
    }

    if (request.count < 2) {
        log_info("capture", "v4l2: only %u buffers granted", request.count);
        goto fail;
    }

    impl->buffer_count = request.count;

    for (unsigned int i = 0; i < impl->buffer_count; i++) {
        struct v4l2_buffer buffer;

        memset(&buffer, 0, sizeof(buffer));
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = i;

        if (xioctl(impl->fd, VIDIOC_QUERYBUF, &buffer) == -1) {
            log_error("capture", "v4l2 VIDIOC_QUERYBUF: errno=%d (%s)", errno, strerror(errno));
            goto fail;
        }

        impl->buffers[i].length = buffer.length;
        impl->buffers[i].start = mmap(NULL, buffer.length,
                                      PROT_READ | PROT_WRITE,
                                      MAP_SHARED,
                                      impl->fd,
                                      buffer.m.offset);

        if (impl->buffers[i].start == MAP_FAILED) {
            log_error("capture", "v4l2 mmap: errno=%d (%s)", errno, strerror(errno));
            goto fail;
        }
    }

    source->name = impl->name;
    source->width = impl->width;
    source->height = impl->height;
    source->fps = impl->fps;
    source->stride = impl->stride;
    source->format = impl->format;
    source->frame_size = impl->frame_size;
    source->start = v4l2_start;
    source->capture = v4l2_capture;
    source->release = v4l2_release;
    source->close = v4l2_close;
    source->impl = impl;

    log_info("capture", "v4l2: %ux%u %s, stride %u, %u fps, %u buffers", impl->width, impl->height,
           impl->format == V4L2_PIX_FMT_YUYV ? "YUYV" : "YU12",
           impl->stride, impl->fps, impl->buffer_count);

    return source;

fail:
    if (impl->fd >= 0) {
        close(impl->fd);
    }
    free(impl);
    free(source);
    return NULL;
}
