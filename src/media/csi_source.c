#define _POSIX_C_SOURCE 200809L

/*
 * csi_source.c
 *
 * Direct CSI camera capture using rpicam-vid / libcamera-vid over a pipe.
 *
 * Eliminates the v4l2loopback bridge entirely:
 *   Raspberry Pi CSI camera -> rpicam-vid (YUV420) -> pipe -> camstream -> x264
 *
 * Features:
 *   - Automatic discovery of rpicam-vid or libcamera-vid
 *   - Camera sensor conflict detection across running processes
 *   - Robust full-frame reading handling short reads, EINTR, and EOF
 *   - Monotonic microsecond timestamps for every frame
 *   - Safe lifecycle management: terminates child on shutdown (SIGTERM -> SIGKILL)
 *   - Standalone stdin mode for piped pipelines (e.g. rpicam-vid ... | camstream --stdin-yuv420)
 */

#include "video_source.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BUFFER_COUNT 2

struct CsiSource {
    int fd;
    pid_t child_pid;
    int is_stdin;
    int running;

    uint8_t *buffers[BUFFER_COUNT];
    unsigned int active_buf;

    char name[64];
    char bin_path[256];
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t stride;
    uint32_t format;
    size_t frame_size;
    uint64_t sequence;
    int verbose;
};

static uint64_t monotonic_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000ULL + (uint64_t) ts.tv_nsec / 1000ULL;
}

/*
 * Check for conflicting processes that may hold the CSI camera sensor.
 * Returns 0 if clear, -1 if conflict found.
 */
static int check_camera_conflicts(void)
{
    DIR *dir = opendir("/proc");
    if (dir == NULL) {
        return 0; /* Cannot inspect /proc; continue */
    }

    static const char *const conflict_names[] = {
        "rpicam-vid", "rpicam-still", "rpicam-hello", "rpicam-raw",
        "libcamera-vid", "libcamera-still", "libcamera-hello", "libcamera-raw"
    };

    pid_t my_pid = getpid();
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') {
            continue;
        }

        pid_t pid = (pid_t) atoi(entry->d_name);
        if (pid == my_pid || pid == 0) {
            continue;
        }

        char comm_path[300];
        snprintf(comm_path, sizeof(comm_path), "/proc/%.255s/comm", entry->d_name);

        FILE *f = fopen(comm_path, "r");
        if (f == NULL) {
            continue;
        }

        char comm[64] = { 0 };
        if (fgets(comm, sizeof(comm), f) != NULL) {
            size_t len = strlen(comm);
            if (len > 0 && comm[len - 1] == '\n') comm[len - 1] = 0;

            for (size_t i = 0; i < sizeof(conflict_names) / sizeof(conflict_names[0]); i++) {
                if (strcmp(comm, conflict_names[i]) == 0) {
                    fclose(f);
                    closedir(dir);
                    fprintf(stderr,
                            "csi: conflicting camera process detected: PID %d (%s).\n"
                            "     The Raspberry Pi CSI camera sensor can only be accessed by one process at a time.\n"
                            "     Please stop it before starting camstream (e.g. kill %d).\n",
                            (int) pid, comm, (int) pid);
                    return -1;
                }
            }
        }
        fclose(f);
    }

    closedir(dir);
    return 0;
}

/*
 * Locate rpicam-vid or libcamera-vid on system.
 */
static int find_rpicam_binary(const char *explicit_path, char *out_path, size_t out_size)
{
    if (explicit_path != NULL && explicit_path[0] != 0) {
        if (access(explicit_path, X_OK) == 0) {
            snprintf(out_path, out_size, "%s", explicit_path);
            return 0;
        }
        fprintf(stderr, "csi: specified binary '%s' not found or not executable\n", explicit_path);
        return -1;
    }

    static const char *const candidates[] = {
        "/usr/bin/rpicam-vid",
        "/usr/local/bin/rpicam-vid",
        "/usr/bin/libcamera-vid",
        "/usr/local/bin/libcamera-vid"
    };

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (access(candidates[i], X_OK) == 0) {
            snprintf(out_path, out_size, "%.*s", (int) (out_size - 1), candidates[i]);
            return 0;
        }
    }

    /* Fallback: search PATH */
    const char *path_env = getenv("PATH");
    if (path_env != NULL) {
        char *path_copy = strdup(path_env);
        if (path_copy != NULL) {
            char *saveptr = NULL;
            char *dir = strtok_r(path_copy, ":", &saveptr);
            while (dir != NULL) {
                char test_path[512];
                snprintf(test_path, sizeof(test_path), "%s/rpicam-vid", dir);
                if (access(test_path, X_OK) == 0) {
                    snprintf(out_path, out_size, "%.*s", (int) (out_size - 1), test_path);
                    free(path_copy);
                    return 0;
                }
                snprintf(test_path, sizeof(test_path), "%s/libcamera-vid", dir);
                if (access(test_path, X_OK) == 0) {
                    snprintf(out_path, out_size, "%.*s", (int) (out_size - 1), test_path);
                    free(path_copy);
                    return 0;
                }
                dir = strtok_r(NULL, ":", &saveptr);
            }
            free(path_copy);
        }
    }

    fprintf(stderr,
            "csi: neither 'rpicam-vid' nor 'libcamera-vid' was found on this system.\n"
            "     Install the official camera tools with:\n"
            "       sudo apt update && sudo apt install -y rpicam-apps\n");
    return -1;
}

/*
 * Robust full-frame reader: loops until exactly total_bytes have been read.
 * Returns  1 on full frame received
 *          0 on clean EOF (0 bytes read)
 *         -1 on error or truncated frame
 */
static int read_full_frame(int fd, uint8_t *buf, size_t total_bytes,
                           pid_t child_pid, int timeout_ms, volatile int *running)
{
    size_t bytes_read = 0;
    struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };

    while (bytes_read < total_bytes && *running) {
        int poll_ret = poll(&pfd, 1, timeout_ms);

        if (poll_ret < 0) {
            if (errno == EINTR) continue;
            perror("csi: poll error");
            return -1;
        }

        if (poll_ret == 0) {
            /* Timeout: check if child process died */
            if (child_pid > 0) {
                int status = 0;
                pid_t r = waitpid(child_pid, &status, WNOHANG);
                if (r == child_pid) {
                    fprintf(stderr, "csi source: rpicam-vid exited unexpectedly (status %d)\n", status);
                    return -1;
                }
            }
            continue;
        }

        if (pfd.revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t n = read(fd, buf + bytes_read, total_bytes - bytes_read);

            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN) continue;
                perror("csi: read error");
                return -1;
            }

            if (n == 0) {
                /* EOF */
                *running = 0;
                if (bytes_read == 0) {
                    fprintf(stderr, "camera source ended: rpicam-vid pipe closed (EOF)\n");
                } else {
                    fprintf(stderr, "csi source error: incomplete YUV420 frame (read %zu of %zu bytes)\n",
                            bytes_read, total_bytes);
                }
                return -1;
            }

            bytes_read += (size_t) n;
        }
    }

    if (!*running) {
        return 0;
    }

    return (bytes_read == total_bytes) ? 1 : -1;
}

static int csi_start(VideoSource *source)
{
    struct CsiSource *impl = source->impl;

    if (impl->running) {
        return 0;
    }

    if (impl->is_stdin) {
        impl->running = 1;
        printf("csi: reading raw YUV420 frames from standard input (%ux%u @ %u fps, frame size %zu bytes)\n",
               impl->width, impl->height, impl->fps, impl->frame_size);
        return 0;
    }

    /* Spawn rpicam-vid subprocess */
    int pipefds[2];
    if (pipe(pipefds) != 0) {
        perror("csi: pipe creation failed");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("csi: fork failed");
        close(pipefds[0]);
        close(pipefds[1]);
        return -1;
    }

    if (pid == 0) {
        /* Child process: rpicam-vid */
        close(pipefds[0]); /* close read end */

        /* Redirect stdout to write end of pipe */
        if (dup2(pipefds[1], STDOUT_FILENO) == -1) {
            perror("csi: dup2 stdout failed");
            _exit(127);
        }
        if (pipefds[1] != STDOUT_FILENO) {
            close(pipefds[1]);
        }

        /* Redirect stdin from /dev/null */
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }

        /* Stderr remains unchanged so rpicam-vid logs are visible in console */

        char w_str[16], h_str[16], fps_str[16];
        snprintf(w_str, sizeof(w_str), "%u", impl->width);
        snprintf(h_str, sizeof(h_str), "%u", impl->height);
        snprintf(fps_str, sizeof(fps_str), "%u", impl->fps);

        char *const argv[] = {
            impl->bin_path,
            "-t", "0",            /* run indefinitely */
            "-n",                 /* no preview window */
            "--width", w_str,
            "--height", h_str,
            "--framerate", fps_str,
            "--codec", "yuv420",  /* uncompressed I420 */
            "-o", "-",            /* stream raw YUV to stdout */
            NULL
        };

        execv(impl->bin_path, argv);
        perror("csi: execv failed");
        _exit(127);
    }

    /* Parent process */
    close(pipefds[1]); /* close write end */
    impl->fd = pipefds[0];
    impl->child_pid = pid;
    impl->running = 1;

    printf("csi: spawned %s (PID %d) for %ux%u @ %u fps YUV420\n",
           impl->bin_path, (int) pid, impl->width, impl->height, impl->fps);

    return 0;
}

static int csi_capture(VideoSource *source,
                       uint64_t *out_timestamp_us,
                       const uint8_t **out_data,
                       size_t *out_size,
                       uint32_t *out_buffer_index)
{
    struct CsiSource *impl = source->impl;

    if (!impl->running) {
        return -1;
    }

    unsigned int target_buf = impl->active_buf ^ 1;
    uint8_t *dst = impl->buffers[target_buf];

    int ret = read_full_frame(impl->fd, dst, impl->frame_size,
                              impl->child_pid, 100, &impl->running);

    if (ret <= 0) {
        return ret;
    }

    *out_timestamp_us = monotonic_us();
    *out_data = dst;
    *out_size = impl->frame_size;
    *out_buffer_index = target_buf;
    impl->active_buf = target_buf;

    return 1;
}

static void csi_release(VideoSource *source, uint32_t buffer_index)
{
    (void) source;
    (void) buffer_index;
    /* Ping-pong buffers are managed internally by active_buf */
}

static void csi_close(VideoSource *source)
{
    if (source == NULL) return;

    struct CsiSource *impl = source->impl;
    if (impl != NULL) {
        impl->running = 0;

        if (impl->child_pid > 0) {
            /* Graceful shutdown: send SIGTERM */
            kill(impl->child_pid, SIGTERM);

            int status = 0;
            int exited = 0;
            for (int i = 0; i < 5; i++) {
                pid_t r = waitpid(impl->child_pid, &status, WNOHANG);
                if (r == impl->child_pid) {
                    exited = 1;
                    break;
                }
                usleep(100000); /* 100ms */
            }

            if (!exited) {
                /* Force terminate if still running */
                kill(impl->child_pid, SIGKILL);
                waitpid(impl->child_pid, &status, 0);
            }
            impl->child_pid = -1;
        }

        if (impl->fd >= 0 && !impl->is_stdin) {
            close(impl->fd);
            impl->fd = -1;
        }

        for (int i = 0; i < BUFFER_COUNT; i++) {
            free(impl->buffers[i]);
            impl->buffers[i] = NULL;
        }

        free(impl);
        source->impl = NULL;
    }

    free(source);
}

VideoSource *csi_source_create(const char *rpicam_bin,
                               uint32_t width,
                               uint32_t height,
                               uint32_t fps,
                               int verbose)
{
    if (fps == 0) return NULL;

    /* Check for conflicting camera processes before starting */
    if (check_camera_conflicts() != 0) {
        return NULL;
    }

    char resolved_bin[256];
    if (find_rpicam_binary(rpicam_bin, resolved_bin, sizeof(resolved_bin)) != 0) {
        return NULL;
    }

    VideoSource *source = calloc(1, sizeof(*source));
    struct CsiSource *impl = calloc(1, sizeof(*impl));

    if (source == NULL || impl == NULL) {
        free(source);
        free(impl);
        return NULL;
    }

    size_t frame_size = (size_t) width * height * 3 / 2;
    for (int i = 0; i < BUFFER_COUNT; i++) {
        impl->buffers[i] = malloc(frame_size);
        if (impl->buffers[i] == NULL) {
            for (int j = 0; j < i; j++) free(impl->buffers[j]);
            free(impl);
            free(source);
            return NULL;
        }
    }

    impl->fd = -1;
    impl->child_pid = -1;
    impl->is_stdin = 0;
    impl->running = 0;
    impl->width = width;
    impl->height = height;
    impl->fps = fps;
    impl->stride = width;
    impl->format = V4L2_PIX_FMT_YUV420;
    impl->frame_size = frame_size;
    impl->verbose = verbose;
    snprintf(impl->bin_path, sizeof(impl->bin_path), "%s", resolved_bin);
    snprintf(impl->name, sizeof(impl->name), "csi (rpicam-vid)");

    source->name = impl->name;
    source->width = width;
    source->height = height;
    source->fps = fps;
    source->stride = width;
    source->format = V4L2_PIX_FMT_YUV420;
    source->frame_size = frame_size;
    source->start = csi_start;
    source->capture = csi_capture;
    source->release = csi_release;
    source->close = csi_close;
    source->impl = impl;

    return source;
}

VideoSource *stdin_source_create(uint32_t width,
                                 uint32_t height,
                                 uint32_t fps)
{
    if (fps == 0) return NULL;

    VideoSource *source = calloc(1, sizeof(*source));
    struct CsiSource *impl = calloc(1, sizeof(*impl));

    if (source == NULL || impl == NULL) {
        free(source);
        free(impl);
        return NULL;
    }

    size_t frame_size = (size_t) width * height * 3 / 2;
    for (int i = 0; i < BUFFER_COUNT; i++) {
        impl->buffers[i] = malloc(frame_size);
        if (impl->buffers[i] == NULL) {
            for (int j = 0; j < i; j++) free(impl->buffers[j]);
            free(impl);
            free(source);
            return NULL;
        }
    }

    impl->fd = STDIN_FILENO;
    impl->child_pid = -1;
    impl->is_stdin = 1;
    impl->running = 0;
    impl->width = width;
    impl->height = height;
    impl->fps = fps;
    impl->stride = width;
    impl->format = V4L2_PIX_FMT_YUV420;
    impl->frame_size = frame_size;
    snprintf(impl->name, sizeof(impl->name), "stdin (yuv420)");

    source->name = impl->name;
    source->width = width;
    source->height = height;
    source->fps = fps;
    source->stride = width;
    source->format = V4L2_PIX_FMT_YUV420;
    source->frame_size = frame_size;
    source->start = csi_start;
    source->capture = csi_capture;
    source->release = csi_release;
    source->close = csi_close;
    source->impl = impl;

    return source;
}
