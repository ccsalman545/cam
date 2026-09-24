/*
 * video_source.h
 *
 * Common interface for every raw video producer:
 *   - V4L2 capture device (USB webcam, v4l2loopback sink)
 *   - Raspberry Pi CSI camera through rpicam-vid (csi_source.c)
 *   - Raw YUV420 on standard input
 *   - Built in synthetic test pattern generator
 *
 * Contract:
 *   capture() fills 'out' with a pointer to internal storage.
 *   The pointer stays valid until the next capture() call or
 *   until release() is called for that frame. The consumer
 *   must copy the data (for example into the frame hub) before
 *   the next capture() call.
 */
#ifndef MEDIA_VIDEO_SOURCE_H
#define MEDIA_VIDEO_SOURCE_H

#include <stddef.h>
#include <stdint.h>

typedef struct VideoSource VideoSource;

struct VideoSource {
    const char *name;       /* human readable, for logs and /status */
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t stride;
    uint32_t format;        /* V4L2 fourcc: YUYV or YU12 */
    size_t frame_size;

    /*
     * Set by a source that can never deliver again (pipe EOF, child
     * process exited, stalled). capture() then returns -1 and the worker
     * gives up at once instead of retrying a transient error.
     */
    volatile int ended;

    int  (*start)(VideoSource *source);
    /*
     * Returns  1  frame ready (out->data valid)
     *          0  no frame this round (spurious wakeup)
     *         -1  fatal error
     */
    int  (*capture)(VideoSource *source, uint64_t *out_timestamp_us,
                    const uint8_t **out_data, size_t *out_size,
                    uint32_t *out_buffer_index);
    void (*release)(VideoSource *source, uint32_t buffer_index);
    void (*close)(VideoSource *source);

    void *impl;
};

/*
 * Open a V4L2 device. Negotiates YUYV first, then YU12
 * (planar 4:2:0). Returns NULL on failure.
 */
VideoSource *v4l2_source_create(const char *device,
                                uint32_t width,
                                uint32_t height,
                                uint32_t fps);

/*
 * Synthetic color bar generator with a moving marker and a
 * frame counter. Produces YUYV at the requested frame rate,
 * paced on CLOCK_MONOTONIC. Used for development and demo
 * setups without a camera.
 */
VideoSource *test_source_create(uint32_t width,
                                uint32_t height,
                                uint32_t fps);

/*
 * CSI camera source: spawns rpicam-vid (or libcamera-vid) as a subprocess,
 * capturing raw YUV420 (I420) frames through a direct pipe.
 * Zero v4l2loopback, zero FFmpeg, zero shell invocation.
 */
VideoSource *csi_source_create(const char *rpicam_bin,
                               uint32_t width,
                               uint32_t height,
                               uint32_t fps,
                               int verbose);

/*
 * Raw YUV420 stdin pipe source: reads uncompressed WxH YUV420 frames
 * directly from standard input (fd 0).
 */
VideoSource *stdin_source_create(uint32_t width,
                                 uint32_t height,
                                 uint32_t fps);

/*
 * Helper: stop and free any source.
 */
void video_source_close(VideoSource *source);

#endif
