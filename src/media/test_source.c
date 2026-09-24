#define _POSIX_C_SOURCE 200809L

/*
 * test_source.c
 *
 * Synthetic camera that renders SMPTE style color bars, a
 * moving white marker and a frame counter in a pixel font.
 * Output format is YUYV, paced at the requested frame rate on
 * CLOCK_MONOTONIC.
 *
 * Purpose: development and verification without hardware, and
 * a known-good signal source for latency measurements.
 */
#include "video_source.h"

#include <errno.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define TEST_BUFFER_COUNT 3

/*
 * 3x5 pixel font for digits 0 to 9.
 */
static const uint8_t DIGIT_GLYPHS[10][5] = {
    { 0x07, 0x05, 0x05, 0x05, 0x07 }, /* 0 */
    { 0x02, 0x06, 0x02, 0x02, 0x07 }, /* 1 */
    { 0x07, 0x01, 0x07, 0x04, 0x07 }, /* 2 */
    { 0x07, 0x01, 0x07, 0x01, 0x07 }, /* 3 */
    { 0x05, 0x05, 0x07, 0x01, 0x01 }, /* 4 */
    { 0x07, 0x04, 0x07, 0x01, 0x07 }, /* 5 */
    { 0x07, 0x04, 0x07, 0x05, 0x07 }, /* 6 */
    { 0x07, 0x01, 0x02, 0x02, 0x02 }, /* 7 */
    { 0x07, 0x05, 0x07, 0x05, 0x07 }, /* 8 */
    { 0x07, 0x05, 0x07, 0x01, 0x07 }  /* 9 */
};

struct TestSource {
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    size_t frame_size;

    uint8_t *buffers[TEST_BUFFER_COUNT];
    unsigned int current;

    uint64_t next_deadline_us;
    uint64_t sequence;
    char name[32];
};

static uint64_t monotonic_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t) ts.tv_sec * 1000000ULL +
           (uint64_t) ts.tv_nsec / 1000ULL;
}

static void sleep_until(uint64_t deadline_us)
{
    struct timespec ts;

    ts.tv_sec = (time_t) (deadline_us / 1000000ULL);
    ts.tv_nsec = (long) ((deadline_us % 1000000ULL) * 1000ULL);

    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) != 0 &&
           errno == EINTR) {
        /* retry after signal */
    }
}

/*
 * Draw one 3x5 digit scaled by 'scale', YUYV plane, at x0, y0.
 * YUYV: 4 bytes = 2 pixels (Y0 U Y1 V). Correctly handle even/odd x.
 */
static void draw_digit(uint8_t *buffer,
                       uint32_t stride,
                       uint32_t x0,
                       uint32_t y0,
                       uint32_t scale,
                       int digit)
{
    uint32_t width = stride / 2;

    for (uint32_t gy = 0; gy < 5; gy++) {
        uint8_t bits = DIGIT_GLYPHS[digit][gy];

        for (uint32_t gx = 0; gx < 3; gx++) {
            int on = (bits >> (2 - gx)) & 1;
            if (!on) {
                continue;
            }

            for (uint32_t sy = 0; sy < scale; sy++) {
                uint32_t row_y = y0 + gy * scale + sy;
                if (row_y >= 10000) { /* sanity, height checked by caller */
                    continue;
                }
                uint8_t *row = buffer + (size_t) row_y * stride;

                for (uint32_t sx = 0; sx < scale; sx++) {
                    uint32_t x = x0 + gx * scale + sx;

                    if (x >= width) {
                        continue;
                    }

                    uint8_t *mac = row + (size_t) (x / 2) * 4;

                    if ((x & 1) == 0) {
                        mac[0] = 235;   /* Y0 */
                    } else {
                        mac[2] = 235;   /* Y1 */
                    }
                    mac[1] = 128;   /* U */
                    mac[3] = 128;   /* V */
                }
            }
        }
    }
}

static void render_frame(struct TestSource *impl, uint8_t *buffer)
{
    const uint32_t width = impl->width;
    const uint32_t height = impl->height;
    const uint32_t stride = width * 2;

    /*
     * Eight 75 percent color bars, expressed in YCbCr.
     * YUYV layout per macropixel: Y0 U Y1 V.
     */
    static const struct { uint8_t y, u, v; } BARS[8] = {
        { 180, 128, 128 },  /* gray white */
        { 168,  44, 136 },  /* yellow */
        { 145,  72,  54 },  /* cyan */
        { 133,  84,  72 },  /* green */
        {  63, 172,  72 },  /* magenta */
        {  51, 184,  54 },  /* red */
        {  28, 212, 114 },  /* blue */
        {  16, 128, 128 }   /* black */
    };

    const uint32_t bar_w = width / 8;
    const uint32_t bars_h = height * 3 / 4;

    for (uint32_t row = 0; row < bars_h; row++) {
        uint8_t *out = buffer + (size_t) row * stride;

        for (uint32_t x = 0; x < width / 2; x++) {
            const unsigned bar = (x * 2) / bar_w;
            const unsigned idx = bar < 8 ? bar : 7;

            out[0] = BARS[idx].y;
            out[1] = BARS[idx].u;
            out[2] = BARS[idx].y;
            out[3] = BARS[idx].v;
            out += 4;
        }
    }

    /*
     * Bottom strip: dark background.
     */
    for (uint32_t row = bars_h; row < height; row++) {
        uint8_t *out = buffer + (size_t) row * stride;

        for (uint32_t x = 0; x < width / 2; x++) {
            out[0] = 32;
            out[1] = 128;
            out[2] = 32;
            out[3] = 128;
            out += 4;
        }
    }

    /*
     * Moving marker: a white square that sweeps the strip once
     * every 'fps' frames. It gives an obvious visual cue for
     * latency and frame drops.
     */
    const uint32_t marker_size = height / 12;
    const uint32_t marker_row = bars_h + (height - bars_h) / 2 - marker_size / 2;
    const uint32_t sweep_steps = impl->fps > 1 ? impl->fps - 1 : 1;
    uint32_t marker_x =
        (uint32_t) (((impl->sequence % impl->fps) * (width - marker_size)) /
                    sweep_steps);
    marker_x &= ~1u; /* YUYV macropixel is 2 pixels, keep even for correct UV */

    for (uint32_t row = 0; row < marker_size && marker_row + row < height; row++) {
        uint8_t *out = buffer + (size_t) (marker_row + row) * stride;

        for (uint32_t x = 0; x < marker_size && marker_x + x + 1 < width; x += 2) {
            uint8_t *pix = out + (size_t) (marker_x + x) * 2;
            pix[0] = 235; /* Y0 */
            pix[1] = 128; /* U */
            pix[2] = 235; /* Y1 */
            pix[3] = 128; /* V */
        }
    }

    /*
     * Frame counter, bottom right, pixel font scaled 4x.
     */
    char digits[12];
    int count = snprintf(digits, sizeof(digits), "%llu",
                         (unsigned long long) impl->sequence);

    const uint32_t scale = height / 96;
    const uint32_t glyph_w = 3 * scale + scale;
    uint32_t x0 = width - (uint32_t) count * glyph_w - scale * 2;
    uint32_t y0 = bars_h + (height - bars_h - 5 * scale) / 2;

    for (int i = 0; i < count; i++) {
        if (digits[i] >= '0' && digits[i] <= '9') {
            draw_digit(buffer, stride, x0, y0, scale, digits[i] - '0');
        }
        x0 += glyph_w;
    }
}

static int test_start(VideoSource *source)
{
    struct TestSource *impl = source->impl;

    impl->next_deadline_us = monotonic_us();

    return 0;
}

static int test_capture(VideoSource *source,
                        uint64_t *out_timestamp_us,
                        const uint8_t **out_data,
                        size_t *out_size,
                        uint32_t *out_buffer_index)
{
    struct TestSource *impl = source->impl;

    const uint64_t period_us = 1000000ULL / impl->fps;

    impl->next_deadline_us += period_us;

    uint64_t now = monotonic_us();

    if (impl->next_deadline_us < now) {
        /*
         * We fell behind (for example a debugger or heavy load).
         * Re-anchor the schedule instead of bursting.
         */
        impl->next_deadline_us = now + period_us;
    }

    sleep_until(impl->next_deadline_us);

    impl->current = (impl->current + 1) % TEST_BUFFER_COUNT;

    render_frame(impl, impl->buffers[impl->current]);

    *out_timestamp_us = monotonic_us();
    *out_data = impl->buffers[impl->current];
    *out_size = impl->frame_size;
    *out_buffer_index = impl->current;

    impl->sequence++;

    return 1;
}

static void test_release(VideoSource *source, uint32_t buffer_index)
{
    (void) source;
    (void) buffer_index;
}

static void test_close(VideoSource *source)
{
    if (source == NULL) {
        return;
    }

    struct TestSource *impl = source->impl;

    if (impl != NULL) {
        for (size_t i = 0; i < TEST_BUFFER_COUNT; i++) {
            free(impl->buffers[i]);
        }
        free(impl);
    }

    free(source);
}

VideoSource *test_source_create(uint32_t width, uint32_t height, uint32_t fps)
{
    if (width < 64 || height < 48 || fps == 0) {
        return NULL;
    }

    VideoSource *source = calloc(1, sizeof(*source));
    struct TestSource *impl = calloc(1, sizeof(*impl));

    if (source == NULL || impl == NULL) {
        free(source);
        free(impl);
        return NULL;
    }

    impl->width = width & ~1u;          /* YUYV needs even dimensions */
    impl->height = height & ~1u;
    impl->fps = fps;
    impl->frame_size = (size_t) impl->width * impl->height * 2;

    for (size_t i = 0; i < TEST_BUFFER_COUNT; i++) {
        impl->buffers[i] = malloc(impl->frame_size);

        if (impl->buffers[i] == NULL) {
            for (size_t j = 0; j < TEST_BUFFER_COUNT; j++) {
                free(impl->buffers[j]);
            }
            free(impl);
            free(source);
            return NULL;
        }
    }

    snprintf(impl->name, sizeof(impl->name), "test pattern");

    source->name = impl->name;
    source->width = impl->width;
    source->height = impl->height;
    source->fps = impl->fps;
    source->stride = impl->width * 2;
    source->format = V4L2_PIX_FMT_YUYV;
    source->frame_size = impl->frame_size;
    source->start = test_start;
    source->capture = test_capture;
    source->release = test_release;
    source->close = test_close;
    source->impl = impl;

    return source;
}

void video_source_close(VideoSource *source)
{
    if (source != NULL && source->close != NULL) {
        source->close(source);
    }
}
