/*
 * test_csi_source.c
 *
 * Unit tests for raw YUV420 stdin and CSI camera pipe sources.
 */

#include "video_source.h"

#include <assert.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define TEST_WIDTH  64
#define TEST_HEIGHT 48
#define TEST_FPS    30
#define TEST_FRAME_SIZE (TEST_WIDTH * TEST_HEIGHT * 3 / 2)

static void test_stdin_source_basic(void)
{
    printf("test_csi_source: stdin_source_basic... ");

    int pipe_fds[2];
    assert(pipe(pipe_fds) == 0);

    /* Save original stdin and replace with pipe_fds[0] */
    int saved_stdin = dup(STDIN_FILENO);
    assert(saved_stdin >= 0);
    assert(dup2(pipe_fds[0], STDIN_FILENO) >= 0);
    close(pipe_fds[0]);

    VideoSource *src = stdin_source_create(TEST_WIDTH, TEST_HEIGHT, TEST_FPS);
    assert(src != NULL);
    assert(strcmp(src->name, "stdin (yuv420)") == 0);
    assert(src->width == TEST_WIDTH);
    assert(src->height == TEST_HEIGHT);
    assert(src->fps == TEST_FPS);
    assert(src->format == V4L2_PIX_FMT_YUV420);

    assert(src->start(src) == 0);

    /* Write one frame in two chunks to test partial read handling */
    uint8_t *sample_frame = malloc(TEST_FRAME_SIZE);
    assert(sample_frame != NULL);
    for (size_t i = 0; i < TEST_FRAME_SIZE; i++) {
        sample_frame[i] = (uint8_t)(i & 0xFF);
    }

    size_t chunk1 = 1000;
    size_t chunk2 = TEST_FRAME_SIZE - chunk1;
    ssize_t w1 = write(pipe_fds[1], sample_frame, chunk1);
    assert(w1 == (ssize_t)chunk1);

    /* Write second chunk */
    ssize_t w2 = write(pipe_fds[1], sample_frame + chunk1, chunk2);
    assert(w2 == (ssize_t)chunk2);

    uint64_t ts = 0;
    const uint8_t *data = NULL;
    size_t size = 0;
    uint32_t buf_idx = 0;

    int rc = src->capture(src, &ts, &data, &size, &buf_idx);
    assert(rc == 1);
    assert(data != NULL);
    assert(size == TEST_FRAME_SIZE);
    assert(ts > 0);
    assert(memcmp(data, sample_frame, TEST_FRAME_SIZE) == 0);

    src->release(src, buf_idx);

    /* Close write end to test EOF handling */
    close(pipe_fds[1]);

    rc = src->capture(src, &ts, &data, &size, &buf_idx);
    assert(rc == -1);

    video_source_close(src);

    /* Restore stdin */
    assert(dup2(saved_stdin, STDIN_FILENO) >= 0);
    close(saved_stdin);
    free(sample_frame);

    printf("passed\n");
}

static void test_csi_source_missing_binary(void)
{
    printf("test_csi_source: csi_source_missing_binary... ");

    /* Passing a nonexistent binary should return NULL gracefully */
    VideoSource *src = csi_source_create("/nonexistent/binary/path",
                                         TEST_WIDTH, TEST_HEIGHT, TEST_FPS, 0);
    assert(src == NULL);

    printf("passed\n");
}

static void test_csi_source_mock_binary(void)
{
    printf("test_csi_source: csi_source_mock_binary... ");

    /* Create a small mock script */
    char mock_script[] = "/tmp/test_mock_rpicam_XXXXXX";
    int fd = mkstemp(mock_script);
    assert(fd >= 0);

    const char *script_content =
        "#!/bin/sh\n"
        "# Generate exactly 2 frames of 64x48 YUV420 (4608 bytes each) = 9216 bytes\n"
        "dd if=/dev/zero bs=4608 count=2 2>/dev/null\n";

    assert(write(fd, script_content, strlen(script_content)) == (ssize_t)strlen(script_content));
    close(fd);
    assert(chmod(mock_script, 0755) == 0);

    VideoSource *src = csi_source_create(mock_script, TEST_WIDTH, TEST_HEIGHT, TEST_FPS, 0);
    assert(src != NULL);
    assert(src->width == TEST_WIDTH);
    assert(src->height == TEST_HEIGHT);
    assert(src->format == V4L2_PIX_FMT_YUV420);

    assert(src->start(src) == 0);

    uint64_t ts = 0;
    const uint8_t *data = NULL;
    size_t size = 0;
    uint32_t buf_idx = 0;

    /* Read frame 1 */
    int rc = src->capture(src, &ts, &data, &size, &buf_idx);
    assert(rc == 1);
    assert(size == TEST_FRAME_SIZE);
    src->release(src, buf_idx);

    /* Read frame 2 */
    rc = src->capture(src, &ts, &data, &size, &buf_idx);
    assert(rc == 1);
    assert(size == TEST_FRAME_SIZE);
    src->release(src, buf_idx);

    /* Frame 3 should encounter EOF */
    rc = src->capture(src, &ts, &data, &size, &buf_idx);
    assert(rc == -1);

    video_source_close(src);

    unlink(mock_script);
    printf("passed\n");
}

int main(void)
{
    test_stdin_source_basic();
    test_csi_source_missing_binary();
    test_csi_source_mock_binary();
    printf("all csi_source checks passed\n");
    return 0;
}
