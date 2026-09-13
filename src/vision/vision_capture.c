#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "frame_hub.h"
#include "source_worker.h"
#include "video_source.h"
#include "vision_worker.h"

static volatile sig_atomic_t stop_flag;
static void on_signal(int signal_number) { (void) signal_number; stop_flag = 1; }

static void usage(const char *name)
{
    fprintf(stderr, "usage: %s [--test | --device PATH] [-W width] [-H height] [-F fps] [-s seconds] [-o prefix]\n", name);
}

int main(int argc, char **argv)
{
    const char *device = "/dev/video0", *prefix = "build/mosaic";
    int test = 0, seconds = 10;
    uint32_t width = 640, height = 480, fps = 30;
    for (int i = 1; i < argc; i++) {
        const char *value = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(argv[i], "--test") == 0) test = 1;
        else if (strcmp(argv[i], "--device") == 0 && value) device = value, i++;
        else if (strcmp(argv[i], "-W") == 0 && value) width = (uint32_t) strtoul(value, NULL, 10), i++;
        else if (strcmp(argv[i], "-H") == 0 && value) height = (uint32_t) strtoul(value, NULL, 10), i++;
        else if (strcmp(argv[i], "-F") == 0 && value) fps = (uint32_t) strtoul(value, NULL, 10), i++;
        else if (strcmp(argv[i], "-s") == 0 && value) seconds = atoi(value), i++;
        else if (strcmp(argv[i], "-o") == 0 && value) prefix = value, i++;
        else { usage(argv[0]); return 2; }
    }
    if (width == 0 || height == 0 || fps == 0 || seconds < 0) { usage(argv[0]); return 2; }
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal);

    VideoSource *source = test ? test_source_create(width, height, fps) : v4l2_source_create(device, width, height, fps);
    if (source == NULL) return 1;
    size_t capacity = (size_t) width * height * 2U;
    FrameHub *hub = frame_hub_create(capacity, 12);
    VisionWorker *vision = hub ? vision_worker_create(hub, width, height, prefix, fps) : NULL;
    SourceWorker *capture = hub ? source_worker_create(source, hub) : NULL;
    if (hub == NULL || vision == NULL || capture == NULL || vision_worker_start(vision) != 0 || source_worker_start(capture) != 0) {
        fprintf(stderr, "vision-capture: unable to start pipeline\n");
        if (capture) source_worker_destroy(capture);
        if (vision) vision_worker_destroy(vision);
        if (hub) frame_hub_destroy(hub);
        video_source_close(source);
        return 1;
    }
    printf("vision-capture: processing %ux%u @ %u fps\n", width, height, fps);
    for (int elapsed = 0; !stop_flag && (seconds == 0 || elapsed < seconds); elapsed++) sleep(1);
    source_worker_stop(capture); vision_worker_stop(vision);
    source_worker_join(capture); vision_worker_join(vision);
    printf("vision-capture: processed %llu frames\n", (unsigned long long) vision_worker_frames_processed(vision));
    source_worker_destroy(capture); vision_worker_destroy(vision); frame_hub_destroy(hub); video_source_close(source);
    return 0;
}
