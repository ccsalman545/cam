#define _POSIX_C_SOURCE 200809L

#include "vision_worker.h"

#include <linux/videodev2.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "frame_matrix.h"

struct VisionWorker {
    FrameHub *hub;
    FrameHubConsumer *consumer;
    uint32_t width, height, output_interval;
    char prefix[256];
    pthread_t thread;
    atomic_int running;
    int started;
    uint64_t processed;
    uint8_t *previous_data;
    uint8_t *current_data;
    uint8_t *mosaic_data;
    VisionMosaic *mosaic;
};

static int copy_gray(const Frame *frame, uint8_t *out, uint32_t width, uint32_t height)
{
    GrayFrame target = { width, height, width, out };
    if (frame->format == V4L2_PIX_FMT_YUYV) {
        return vision_gray_from_yuyv(frame->data, frame->stride, &target);
    }
    if (frame->format == V4L2_PIX_FMT_YUV420 || frame->format == V4L2_PIX_FMT_YUV420M) {
        if (frame->stride < width || frame->size < (size_t) frame->stride * height) return -1;
        for (uint32_t y = 0; y < height; y++) {
            memcpy(out + (size_t) y * width,
                   frame->data + (size_t) y * frame->stride, width);
        }
        return 0;
    }
    return -1;
}

static void write_outputs(VisionWorker *w)
{
    char path[320];
    snprintf(path, sizeof(path), "%s.pgm", w->prefix);
    if (vision_mosaic_write_pgm(w->mosaic, path) != 0) fprintf(stderr, "vision: cannot write %s\n", path);
    snprintf(path, sizeof(path), "%s.obj", w->prefix);
    if (vision_mosaic_write_obj(w->mosaic, path, 1.0f) != 0) fprintf(stderr, "vision: cannot write %s\n", path);
}

static void *vision_thread(void *arg)
{
    VisionWorker *w = arg;
    int have_previous = 0;
    int origin_x = (int) w->width;
    int origin_y = (int) w->height;
    GrayFrame previous = { w->width, w->height, w->width, w->previous_data };
    GrayFrame current = { w->width, w->height, w->width, w->current_data };

    while (w->running) {
        Frame *frame = frame_hub_take(w->consumer);
        if (frame == NULL) { usleep(2000); continue; }
        if (copy_gray(frame, w->current_data, w->width, w->height) == 0) {
            if (!have_previous) {
                vision_mosaic_add(w->mosaic, &current, origin_x, origin_y);
                have_previous = 1;
            } else {
                int dx = 0, dy = 0;
                if (vision_estimate_offset(&previous, &current, 32, 32, &dx, &dy, NULL) == 0) {
                    origin_x += dx;
                    origin_y += dy;
                    vision_mosaic_add(w->mosaic, &current, origin_x, origin_y);
                }
            }
            memcpy(w->previous_data, w->current_data, (size_t) w->width * w->height);
            w->processed++;
            if (w->output_interval != 0 && w->processed % w->output_interval == 0) write_outputs(w);
        }
        frame_unref(frame_hub_pool(w->hub), frame);
    }
    return NULL;
}

VisionWorker *vision_worker_create(FrameHub *hub, uint32_t width, uint32_t height,
                                   const char *prefix, uint32_t output_interval)
{
    if (hub == NULL || width == 0 || height == 0 || prefix == NULL || prefix[0] == '\0') return NULL;
    VisionWorker *w = calloc(1, sizeof(*w));
    if (w == NULL) return NULL;
    size_t pixels = (size_t) width * height;
    w->previous_data = malloc(pixels);
    w->current_data = malloc(pixels);
    w->mosaic_data = NULL;
    w->mosaic = vision_mosaic_create(width * 3U, height * 3U);
    w->consumer = frame_hub_subscribe(hub);
    if (w->previous_data == NULL || w->current_data == NULL || w->mosaic == NULL || w->consumer == NULL) {
        if (w->consumer) frame_hub_unsubscribe(hub, w->consumer);
        vision_mosaic_destroy(w->mosaic); free(w->previous_data); free(w->current_data); free(w); return NULL;
    }
    w->hub = hub; w->width = width; w->height = height; w->output_interval = output_interval;
    snprintf(w->prefix, sizeof(w->prefix), "%s", prefix);
    return w;
}

int vision_worker_start(VisionWorker *w)
{
    if (w == NULL || w->started) return -1;
    w->running = 1;
    if (pthread_create(&w->thread, NULL, vision_thread, w) != 0) return -1;
    w->started = 1;
    return 0;
}
void vision_worker_stop(VisionWorker *w) { if (w) w->running = 0; }
void vision_worker_join(VisionWorker *w) { if (w && w->started) { pthread_join(w->thread, NULL); w->started = 0; } }
uint64_t vision_worker_frames_processed(const VisionWorker *w) { return w ? w->processed : 0; }
void vision_worker_destroy(VisionWorker *w)
{
    if (w == NULL) return;
    vision_worker_stop(w); vision_worker_join(w);
    frame_hub_unsubscribe(w->hub, w->consumer);
    write_outputs(w);
    vision_mosaic_destroy(w->mosaic); free(w->previous_data); free(w->current_data); free(w);
}
