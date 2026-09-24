#define _GNU_SOURCE

/*
 * csi_source.c
 *
 * Raspberry Pi CSI camera capture through rpicam-vid (or the older
 * libcamera-vid) over a pipe:
 *
 *   sensor -> libcamera ISP -> rpicam-vid --codec yuv420 -> pipe -> camstream
 *
 * This is the supported path for libcamera sensors such as the IMX219:
 * their /dev/video0 is the raw Unicam/CFE node, which only delivers
 * Bayer data after the media graph has been configured by libcamera,
 * so direct V4L2 YUYV capture from it fails (VIDIOC_STREAMON EINVAL).
 *
 * Frame layout. rpicam-vid writes the ISP buffer as is: I420 with every
 * luma row padded to a multiple of 64 bytes and every chroma row to 32.
 * The source therefore reports stride = align64(width) and a frame size
 * of stride * height * 3 / 2; the encode worker de-pads when the stride
 * differs from the width. Widths that are a multiple of 64 (640, 1280,
 * 1920) have no padding at all.
 *
 * The stdin source ("rpicam-vid ... -o - | camstream --stdin-yuv420")
 * reads packed WxH I420 frames: use a width that is a multiple of 64
 * when the producer is rpicam-vid.
 *
 * Process handling: the child is started with posix_spawn (no code runs
 * between fork and exec, so no lock can be inherited in a locked state),
 * every camstream descriptor is close-on-exec (the child must not keep
 * the HTTP port or the media sockets open if camstream dies), and the
 * child is reaped exactly once.
 */

#include "video_source.h"

#include "log.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

/*
 * A pipe holds 64 KB by default, a 720p frame is 1.3 MB: with the
 * default size rpicam-vid blocks many times per frame. 1 MB is the
 * unprivileged maximum (/proc/sys/fs/pipe-max-size).
 */
#define PIPE_SIZE (1024 * 1024)

/* No byte for this long means rpicam-vid or the camera hung. */
#define FIRST_FRAME_TIMEOUT_MS 15000    /* libcamera start-up included */
#define FRAME_TIMEOUT_MS 5000

#define POLL_SLICE_MS 100
#define TERMINATE_WAIT_MS 1000

struct CsiSource {
    int fd;
    pid_t child_pid;                /* -1 when none or already reaped */
    int is_stdin;
    int running;
    int have_frame;                 /* a complete frame arrived */

    uint8_t *buffer;

    char name[64];
    char bin_path[256];
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t stride;
    size_t frame_size;
    int verbose;
};

static uint64_t monotonic_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000ULL + (uint64_t) ts.tv_nsec / 1000ULL;
}

static uint64_t monotonic_ms(void)
{
    return monotonic_us() / 1000ULL;
}

static void describe_exit(int status, char *out, size_t size)
{
    if (WIFEXITED(status)) {
        snprintf(out, size, "exit status %d", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        snprintf(out, size, "killed by signal %d", WTERMSIG(status));
    } else {
        snprintf(out, size, "status 0x%x", (unsigned) status);
    }
}

/* Reap the child if it has exited. Returns 1 when it was reaped. */
static int reap_child(struct CsiSource *impl, int log_it)
{
    if (impl->child_pid <= 0) {
        return 0;
    }

    int status = 0;

    if (waitpid(impl->child_pid, &status, WNOHANG) != impl->child_pid) {
        return 0;
    }

    if (log_it) {
        char how[48];

        describe_exit(status, how, sizeof(how));
        log_error("capture", "csi: %s (PID %d) exited: %s; see its messages "
                             "above (camera connected? 'rpicam-hello "
                             "--list-cameras')",
                  impl->bin_path, (int) impl->child_pid, how);
    }

    impl->child_pid = -1;
    return 1;
}

/*
 * The CSI sensor has a single owner. Another rpicam/libcamera app (or a
 * second camstream) makes rpicam-vid fail with "pipeline handler in use"
 * a second later; saying so up front is clearer.
 */
static int check_camera_conflicts(void)
{
    DIR *dir = opendir("/proc");

    if (dir == NULL) {
        return 0;
    }

    static const char *const conflict_names[] = {
        "rpicam-vid", "rpicam-still", "rpicam-hello", "rpicam-raw",
        "rpicam-jpeg", "libcamera-vid", "libcamera-still", "libcamera-hello",
        "libcamera-raw", "libcamera-jpeg"
    };

    struct dirent *entry;
    int conflict = 0;

    while (!conflict && (entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') {
            continue;
        }

        char comm_path[300];

        snprintf(comm_path, sizeof(comm_path), "/proc/%.255s/comm",
                 entry->d_name);

        FILE *f = fopen(comm_path, "re");

        if (f == NULL) {
            continue;
        }

        char comm[64] = { 0 };

        if (fgets(comm, sizeof(comm), f) != NULL) {
            comm[strcspn(comm, "\n")] = 0;

            for (size_t i = 0;
                 i < sizeof(conflict_names) / sizeof(conflict_names[0]); i++) {
                if (strcmp(comm, conflict_names[i]) == 0) {
                    log_error("capture", "csi: camera busy: PID %s (%s) holds "
                                         "it; stop it first (kill %s)",
                              entry->d_name, comm, entry->d_name);
                    conflict = 1;
                    break;
                }
            }
        }

        fclose(f);
    }

    closedir(dir);
    return conflict ? -1 : 0;
}

static int find_rpicam_binary(const char *explicit_path, char *out_path,
                              size_t out_size)
{
    if (explicit_path != NULL && explicit_path[0] != 0) {
        if (access(explicit_path, X_OK) == 0) {
            snprintf(out_path, out_size, "%s", explicit_path);
            return 0;
        }

        log_error("capture", "csi: specified binary '%s' not found or not "
                             "executable", explicit_path);
        return -1;
    }

    static const char *const names[] = { "rpicam-vid", "libcamera-vid" };
    const char *path_env = getenv("PATH");
    char path_copy[1024];

    snprintf(path_copy, sizeof(path_copy), "%s:/usr/bin:/usr/local/bin",
             path_env != NULL ? path_env : "");

    for (size_t n = 0; n < sizeof(names) / sizeof(names[0]); n++) {
        char *saveptr = NULL;
        char scan[sizeof(path_copy)];

        memcpy(scan, path_copy, sizeof(scan));

        for (char *dir = strtok_r(scan, ":", &saveptr); dir != NULL;
             dir = strtok_r(NULL, ":", &saveptr)) {
            char candidate[512];

            if (dir[0] == 0) {
                continue;
            }

            snprintf(candidate, sizeof(candidate), "%s/%s", dir, names[n]);

            size_t length = strlen(candidate);

            if (length < out_size && access(candidate, X_OK) == 0) {
                memcpy(out_path, candidate, length + 1);
                return 0;
            }
        }
    }

    log_error("capture", "csi: neither rpicam-vid nor libcamera-vid found; "
                         "install them with: sudo apt install -y rpicam-apps");
    return -1;
}

/*
 * Read exactly one frame.
 * Returns  1 full frame
 *          0 stopped (running cleared by close)
 *         -1 EOF, error, child exit or stall (the source is finished)
 */
static int read_full_frame(struct CsiSource *impl)
{
    size_t bytes_read = 0;
    uint64_t last_data_ms = monotonic_ms();
    uint64_t timeout_ms = impl->have_frame ? FRAME_TIMEOUT_MS
                                           : FIRST_FRAME_TIMEOUT_MS;
    struct pollfd pfd = { .fd = impl->fd, .events = POLLIN, .revents = 0 };

    while (bytes_read < impl->frame_size && impl->running) {
        int ready = poll(&pfd, 1, POLL_SLICE_MS);

        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            log_error("capture", "csi: poll: %s", strerror(errno));
            return -1;
        }

        if (ready == 0) {
            if (reap_child(impl, 1)) {
                return -1;
            }

            if (monotonic_ms() - last_data_ms >= timeout_ms) {
                log_error("capture", "csi: no %s from %s for %llu s; camera "
                                     "or libcamera stalled",
                          impl->have_frame ? "data" : "first frame",
                          impl->is_stdin ? "stdin" : impl->bin_path,
                          (unsigned long long) (timeout_ms / 1000));
                return -1;
            }
            continue;
        }

        ssize_t n = read(impl->fd, impl->buffer + bytes_read,
                         impl->frame_size - bytes_read);

        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            log_error("capture", "csi: read: %s", strerror(errno));
            return -1;
        }

        if (n == 0) {
            if (bytes_read == 0) {
                log_error("capture", "camera source ended: %s pipe closed "
                                     "(EOF)",
                          impl->is_stdin ? "stdin" : "rpicam-vid");
            } else {
                log_error("capture", "csi: truncated frame (%zu of %zu "
                                     "bytes): width/height do not match the "
                                     "producer's output",
                          bytes_read, impl->frame_size);
            }

            /* Give a dying child a moment so its exit status is logged. */
            for (int i = 0; i < 5 && impl->child_pid > 0; i++) {
                if (reap_child(impl, 1)) {
                    break;
                }
                poll(NULL, 0, 20);
            }
            return -1;
        }

        bytes_read += (size_t) n;
        last_data_ms = monotonic_ms();
    }

    if (!impl->running) {
        return 0;
    }

    impl->have_frame = 1;
    return 1;
}

static int spawn_rpicam(struct CsiSource *impl)
{
    int pipefds[2];

    if (pipe2(pipefds, O_CLOEXEC) != 0) {
        log_error("capture", "csi: pipe: %s", strerror(errno));
        return -1;
    }

    if (fcntl(pipefds[0], F_SETPIPE_SZ, PIPE_SIZE) < 0) {
        log_debug("capture", "csi: F_SETPIPE_SZ %d failed: %s", PIPE_SIZE,
                  strerror(errno));
    }

    char w_str[16], h_str[16], fps_str[16];

    snprintf(w_str, sizeof(w_str), "%u", impl->width);
    snprintf(h_str, sizeof(h_str), "%u", impl->height);
    snprintf(fps_str, sizeof(fps_str), "%u", impl->fps);

    char *const argv[] = {
        impl->bin_path,
        "-t", "0",              /* run until stopped */
        "-n",                   /* no preview window */
        "--width", w_str,
        "--height", h_str,
        "--framerate", fps_str,
        "--codec", "yuv420",    /* uncompressed I420, rows padded */
        "--flush",              /* write each frame at once */
        "-o", "-",
        NULL
    };

    /*
     * libcamera logs every INFO line (sensor modes, tuning file) to
     * stderr, i.e. the journal. Keep warnings and errors unless the
     * operator asked for verbose output or set the variable already.
     */
    int quiet = !impl->verbose && getenv("LIBCAMERA_LOG_LEVELS") == NULL;
    size_t env_count = 0;

    while (environ != NULL && environ[env_count] != NULL) {
        env_count++;
    }

    char **envp = calloc(env_count + 2, sizeof(*envp));

    if (envp == NULL) {
        close(pipefds[0]);
        close(pipefds[1]);
        return -1;
    }

    for (size_t i = 0; i < env_count; i++) {
        envp[i] = environ[i];
    }

    static char quiet_env[] = "LIBCAMERA_LOG_LEVELS=*:WARN";

    if (quiet) {
        envp[env_count] = quiet_env;
    }

    posix_spawn_file_actions_t actions;
    int rc = posix_spawn_file_actions_init(&actions);

    if (rc == 0) {
        /* dup2 clears close-on-exec on the new fd 1. */
        rc = posix_spawn_file_actions_adddup2(&actions, pipefds[1],
                                              STDOUT_FILENO);
    }
    if (rc == 0) {
        rc = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO,
                                              "/dev/null", O_RDONLY, 0);
    }

    pid_t pid = -1;

    if (rc == 0) {
        rc = posix_spawn(&pid, impl->bin_path, &actions, NULL, argv, envp);
    }

    posix_spawn_file_actions_destroy(&actions);
    free(envp);
    close(pipefds[1]);

    if (rc != 0) {
        log_error("capture", "csi: cannot start %s: %s", impl->bin_path,
                  strerror(rc));
        close(pipefds[0]);
        return -1;
    }

    impl->fd = pipefds[0];
    impl->child_pid = pid;

    log_info("capture", "csi: started %s (PID %d): %ux%u @ %u fps YUV420, "
                        "stride %u, %zu byte frames",
             impl->bin_path, (int) pid, impl->width, impl->height, impl->fps,
             impl->stride, impl->frame_size);

    return 0;
}

static int csi_start(VideoSource *source)
{
    struct CsiSource *impl = source->impl;

    if (impl->running) {
        return 0;
    }

    if (impl->is_stdin) {
        log_info("capture", "csi: reading raw YUV420 frames from standard "
                            "input (%ux%u @ %u fps, frame size %zu bytes)",
                 impl->width, impl->height, impl->fps, impl->frame_size);
    } else if (spawn_rpicam(impl) != 0) {
        return -1;
    }

    impl->running = 1;
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

    int ret = read_full_frame(impl);

    if (ret < 0) {
        impl->running = 0;
        source->ended = 1;
    }

    if (ret <= 0) {
        return ret;
    }

    /*
     * Arrival time. rpicam-vid writes a frame as soon as the ISP has
     * finished it, so this trails the sensor exposure by a constant
     * ISP + pipe delay; what matters for RTP is that it is monotonic
     * and evenly spaced.
     */
    *out_timestamp_us = monotonic_us();
    *out_data = impl->buffer;
    *out_size = impl->frame_size;
    *out_buffer_index = 0;

    return 1;
}

static void csi_release(VideoSource *source, uint32_t buffer_index)
{
    /* The frame hub copies the frame before release; one buffer is enough. */
    (void) source;
    (void) buffer_index;
}

static void csi_close(VideoSource *source)
{
    if (source == NULL) {
        return;
    }

    struct CsiSource *impl = source->impl;

    if (impl != NULL) {
        impl->running = 0;

        /*
         * Close our end first: a child blocked writing into a full pipe
         * then gets EPIPE at once instead of waiting for the signal.
         */
        if (impl->fd >= 0 && !impl->is_stdin) {
            close(impl->fd);
        }
        impl->fd = -1;

        if (impl->child_pid > 0 && !reap_child(impl, 0)) {
            kill(impl->child_pid, SIGTERM);

            for (int waited = 0; waited < TERMINATE_WAIT_MS &&
                                 !reap_child(impl, 0);
                 waited += 20) {
                poll(NULL, 0, 20);
            }

            if (impl->child_pid > 0) {
                log_warn("capture", "csi: %s (PID %d) ignored SIGTERM; "
                                    "killing it", impl->bin_path,
                         (int) impl->child_pid);
                kill(impl->child_pid, SIGKILL);
                waitpid(impl->child_pid, NULL, 0);
                impl->child_pid = -1;
            }
        }

        free(impl->buffer);
        free(impl);
        source->impl = NULL;
    }

    free(source);
}

static VideoSource *source_alloc(uint32_t width, uint32_t height,
                                 uint32_t fps, uint32_t stride,
                                 struct CsiSource **impl_out)
{
    VideoSource *source = calloc(1, sizeof(*source));
    struct CsiSource *impl = calloc(1, sizeof(*impl));
    size_t frame_size = (size_t) stride * height * 3 / 2;

    if (source == NULL || impl == NULL ||
        (impl->buffer = malloc(frame_size)) == NULL) {
        if (impl != NULL) {
            free(impl->buffer);
        }
        free(impl);
        free(source);
        return NULL;
    }

    impl->fd = -1;
    impl->child_pid = -1;
    impl->width = width;
    impl->height = height;
    impl->fps = fps;
    impl->stride = stride;
    impl->frame_size = frame_size;

    source->width = width;
    source->height = height;
    source->fps = fps;
    source->stride = stride;
    source->format = V4L2_PIX_FMT_YUV420;
    source->frame_size = frame_size;
    source->start = csi_start;
    source->capture = csi_capture;
    source->release = csi_release;
    source->close = csi_close;
    source->impl = impl;
    source->name = impl->name;

    *impl_out = impl;
    return source;
}

static int valid_geometry(uint32_t width, uint32_t height, uint32_t fps)
{
    if (fps == 0 || width < 16 || height < 16 || (width & 1) || (height & 1)) {
        log_error("capture", "csi: %ux%u @ %u fps is not usable (even width "
                             "and height of at least 16 required)",
                  width, height, fps);
        return 0;
    }
    return 1;
}

VideoSource *csi_source_create(const char *rpicam_bin,
                               uint32_t width,
                               uint32_t height,
                               uint32_t fps,
                               int verbose)
{
    if (!valid_geometry(width, height, fps)) {
        return NULL;
    }

    if (check_camera_conflicts() != 0) {
        return NULL;
    }

    char resolved_bin[256];

    if (find_rpicam_binary(rpicam_bin, resolved_bin, sizeof(resolved_bin)) != 0) {
        return NULL;
    }

    /* rpicam-vid pads luma rows to 64 bytes (chroma to 32). */
    uint32_t stride = (width + 63u) & ~63u;

    if (stride != width) {
        log_warn("capture", "csi: width %u is not a multiple of 64: "
                            "rpicam-vid pads rows to %u bytes, which costs a "
                            "copy per frame; prefer 640, 1280 or 1920",
                 width, stride);
    }

    struct CsiSource *impl = NULL;
    VideoSource *source = source_alloc(width, height, fps, stride, &impl);

    if (source == NULL) {
        return NULL;
    }

    impl->verbose = verbose;
    snprintf(impl->bin_path, sizeof(impl->bin_path), "%s", resolved_bin);

    const char *base = strrchr(resolved_bin, '/');

    snprintf(impl->name, sizeof(impl->name), "csi (%.48s)",
             base != NULL ? base + 1 : resolved_bin);

    return source;
}

VideoSource *stdin_source_create(uint32_t width,
                                 uint32_t height,
                                 uint32_t fps)
{
    if (!valid_geometry(width, height, fps)) {
        return NULL;
    }

    struct CsiSource *impl = NULL;
    VideoSource *source = source_alloc(width, height, fps, width, &impl);

    if (source == NULL) {
        return NULL;
    }

    impl->fd = STDIN_FILENO;
    impl->is_stdin = 1;
    snprintf(impl->name, sizeof(impl->name), "stdin (yuv420)");

    return source;
}
