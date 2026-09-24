/*
 * test_encoder_worker.c
 *
 * Standalone checks for the encode worker's drop accounting
 * and stall watchdog.
 *
 *   make test
 *
 * The real H.264 backends are replaced with a stub whose
 * behavior the test switches through encode_mode.
 */
#include "encoder_worker.h"

#include <linux/videodev2.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define WIDTH  640
#define HEIGHT 480

/* ------------------------------------------------------------------ */
/* Encoder stub                                                        */
/* ------------------------------------------------------------------ */

enum EncodeMode {
    ENCODE_OK,          /* produce one access unit per input */
    ENCODE_NO_OUTPUT    /* simulate pipeline depth: never emit */
};

/*
 * Atomic because main switches the mode between phases while the encode
 * thread may still be inside the stub. The sleeps in the test make the
 * switch land between frames in practice, but relying on that would
 * leave a real race in the test itself.
 */
static atomic_int encode_mode = ENCODE_OK;

/* Target bitrate the worker asked the stub to apply, 0 = never asked. */
static atomic_uint stub_bitrate_kbps;

int h264_encoder_set_bitrate(H264Encoder *encoder, uint32_t bitrate_kbps)
{
    (void) encoder;

    if (bitrate_kbps == 0) {
        return -1;
    }

    atomic_store(&stub_bitrate_kbps, bitrate_kbps);
    return 0;
}

int h264_encoder_encode(H264Encoder *encoder,
                        const uint8_t *plane_y,
                        const uint8_t *plane_u,
                        const uint8_t *plane_v,
                        uint64_t pts_us, int force_idr,
                        uint8_t *out, size_t out_capacity,
                        size_t *out_size, int *out_is_idr)
{
    (void) encoder;
    (void) plane_y;
    (void) plane_u;
    (void) plane_v;
    (void) pts_us;
    (void) force_idr;

    if (atomic_load(&encode_mode) == ENCODE_NO_OUTPUT) {
        return 0;
    }

    if (out_capacity < 4) {
        return -1;
    }

    memcpy(out, "\0\0\0\x01", 4);
    *out_size = 4;
    *out_is_idr = 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int failures;

static void check(int condition, const char *what)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    } else {
        printf("ok: %s\n", what);
    }
}

static void wait_for_frames(EncoderWorker *worker, uint64_t target_seen)
{
    for (int i = 0; i < 2000; i++) {
        EncoderWorkerStats stats;
        encoder_worker_get_stats(worker, &stats);

        if (stats.frames_seen >= target_seen) {
            return;
        }

        struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 };
        nanosleep(&ts, NULL);
    }
}

int main(void)
{
    FrameHub *hub = frame_hub_create((size_t) WIDTH * HEIGHT * 2, 6);
    AuRing *ring = au_ring_create(64 * 1024, 8);
    atomic_int force_idr = 0;
    atomic_int active = 0;

    if (hub == NULL || ring == NULL) {
        fprintf(stderr, "FAIL: setup\n");
        return 1;
    }

    /*
     * The stub ignores its encoder handle; the address of the mode flag
     * is only a non-NULL token for the worker.
     */
    EncoderWorker *worker = encoder_worker_create(hub,
                                                  (H264Encoder *) &encode_mode,
                                                  WIDTH, HEIGHT,
                                                  ring,
                                                  &force_idr,
                                                  &active);

    if (worker == NULL || encoder_worker_start(worker) != 0) {
        fprintf(stderr, "FAIL: worker start\n");
        return 1;
    }

    uint8_t *raw = calloc(1, (size_t) WIDTH * HEIGHT * 2);
    uint64_t sequence = 0;

    /*
     * 1. Idle: frames are discarded by design, nothing is
     *    encoded and no stall warning may fire.
     */
    for (int i = 0; i < 20; i++) {
        frame_hub_publish(hub, raw, (size_t) WIDTH * HEIGHT * 2,
                          WIDTH, HEIGHT, V4L2_PIX_FMT_YUYV,
                          WIDTH * 2, sequence++, (uint64_t) i * 33333);
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 3000000 };
        nanosleep(&ts, NULL);
    }
    wait_for_frames(worker, 20);

    EncoderWorkerStats stats;
    encoder_worker_get_stats(worker, &stats);
    check(stats.frames_seen >= 20, "idle frames reached the worker");
    check(stats.frames_encoded == 0, "idle encodes nothing");
    check(stats.skipped_idle >= 20, "idle frames counted as skipped_idle");

    /*
     * 2. Active with matching frames: encoding happens.
     */
    atomic_store(&active, 1);

    uint64_t seen_before = stats.frames_seen;

    for (int i = 0; i < 30; i++) {
        frame_hub_publish(hub, raw, (size_t) WIDTH * HEIGHT * 2,
                          WIDTH, HEIGHT, V4L2_PIX_FMT_YUYV,
                          WIDTH * 2, sequence++, (uint64_t) i * 33333);
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 3000000 };
        nanosleep(&ts, NULL);
    }
    wait_for_frames(worker, seen_before + 30);

    encoder_worker_get_stats(worker, &stats);
    check(stats.frames_encoded > 0, "active window encodes frames");
    check(stats.frames_encoded == stats.frames_seen - stats.skipped_idle -
                                   stats.skipped_mismatch -
                                   stats.skipped_bad_size,
          "drop accounting is exact");
    check(stats.skipped_mismatch == 0, "no mismatch when sizes agree");

    /*
     * 3. Active but wrong dimensions: frames are dropped into
     *    skipped_mismatch and the watchdog reports the stall.
     *    Capture stderr to verify the one-shot diagnostics.
     */
    fflush(stderr);
    int saved_stderr = dup(STDERR_FILENO);
    FILE *capture = tmpfile();
    dup2(fileno(capture), STDERR_FILENO);

    for (int i = 0; i < 120; i++) {
        frame_hub_publish(hub, raw, (size_t) 320 * 240 * 2,
                          320, 240, V4L2_PIX_FMT_YUYV,
                          320 * 2, sequence++, (uint64_t) i * 33333);
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 3000000 };
        nanosleep(&ts, NULL);
    }

    fflush(NULL);
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stderr);

    rewind(capture);
    char log_buffer[8192] = "";
    size_t got = fread(log_buffer, 1, sizeof(log_buffer) - 1, capture);
    log_buffer[got] = 0;
    fclose(capture);

    encoder_worker_get_stats(worker, &stats);
    check(stats.skipped_mismatch > 0, "mismatched frames counted");
    check(strstr(log_buffer, "does not match the encoder") != NULL,
          "mismatch logged once with dimensions");
    check(strstr(log_buffer, "stalled, no output") != NULL,
          "stall watchdog fired with breakdown");

    /*
     * 4. Encoder produces no output (pipeline depth): counted
     *    separately so the status endpoint can name the cause.
     */
    atomic_store(&encode_mode, ENCODE_NO_OUTPUT);
    atomic_store(&active, 0);

    /* Give the worker a chance to observe the idle state so the
     * next activation cleanly re-arms the watchdog. */
    uint64_t seen_after_mismatch = stats.frames_seen;

    for (int i = 0; i < 5; i++) {
        frame_hub_publish(hub, raw, (size_t) WIDTH * HEIGHT * 2,
                          WIDTH, HEIGHT, V4L2_PIX_FMT_YUYV,
                          WIDTH * 2, sequence++, (uint64_t) i * 33333);
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 3000000 };
        nanosleep(&ts, NULL);
    }
    wait_for_frames(worker, seen_after_mismatch + 5);

    encoder_worker_get_stats(worker, &stats);
    atomic_store(&active, 1);

    uint64_t encoded_before = stats.frames_encoded;
    uint64_t seen_before_noout = stats.frames_seen;

    for (int i = 0; i < 10; i++) {
        frame_hub_publish(hub, raw, (size_t) WIDTH * HEIGHT * 2,
                          WIDTH, HEIGHT, V4L2_PIX_FMT_YUYV,
                          WIDTH * 2, sequence++, (uint64_t) i * 33333);
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 3000000 };
        nanosleep(&ts, NULL);
    }
    wait_for_frames(worker, seen_before_noout + 10);

    encoder_worker_get_stats(worker, &stats);
    check(stats.frames_encoded == encoded_before,
          "no output mode encodes nothing");
    check(stats.no_output > 0, "encoder no-output rounds counted");

    /*
     * 5. A bitrate change is queued by the caller and applied by the
     *    encode thread, which is the only thread allowed to touch the
     *    encoder handle.
     */
    atomic_store(&encode_mode, ENCODE_OK);
    atomic_store(&active, 1);

    check(encoder_worker_request_bitrate(worker, 1337) == 0,
          "bitrate request queued");
    check(encoder_worker_request_bitrate(worker, 0) == -1,
          "a zero bitrate request is refused");
    check(encoder_worker_request_bitrate(NULL, 1337) == -1,
          "a request without a worker is refused");

    uint64_t seen_before_bitrate = stats.frames_seen;

    for (int i = 0; i < 10; i++) {
        frame_hub_publish(hub, raw, (size_t) WIDTH * HEIGHT * 2,
                          WIDTH, HEIGHT, V4L2_PIX_FMT_YUYV,
                          WIDTH * 2, sequence++, (uint64_t) i * 33333);
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 3000000 };
        nanosleep(&ts, NULL);
    }
    wait_for_frames(worker, seen_before_bitrate + 10);

    check(atomic_load(&stub_bitrate_kbps) == 1337,
          "encode thread applied the queued bitrate");
    check(encoder_worker_bitrate_kbps(worker) == 1337,
          "applied bitrate is reported back");

    /*
     * 6. Null worker getters behave.
     */
    encoder_worker_get_stats(NULL, &stats);
    check(stats.frames_seen == 0 && stats.frames_encoded == 0,
          "NULL worker yields zeroed stats");

    encoder_worker_stop(worker);
    encoder_worker_destroy(worker);
    au_ring_destroy(ring);
    frame_hub_destroy(hub);
    free(raw);

    if (failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    printf("all encoder worker checks passed\n");
    return 0;
}
