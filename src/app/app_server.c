#define _POSIX_C_SOURCE 200809L

/*
 * app_server.c
 *
 * Composition root and main loop.
 *
 *   main thread    Mongoose HTTP (web UI, signaling, diagnostics,
 *                  recovery), session UDP sockets, SRTP send fan out
 *   source thread  V4L2 / CSI / stdin / test pattern capture into the hub
 *   encode thread  I420 conversion and H.264 encoding into the AU ring
 *
 * The media pipeline is rebuildable at runtime: POST /api/camera/restart
 * tears down source, hub, encoder and the two worker threads, then
 * builds them again from the current configuration. Sessions and their
 * UDP sockets survive a camera restart, so a viewer only sees a gap
 * followed by a fresh keyframe.
 *
 * The management interface is independent of WebRTC: a failure to set
 * up DTLS, a full session table or a dead viewer socket is reported and
 * counted, never fatal. Only an unusable HTTP listener or a fatal
 * camera/encoder error at startup stops the process.
 */
#include "app_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <poll.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <openssl/rand.h>

#include "mongoose.h"

#include "au_ring.h"
#include "dtls_srtp.h"
#include "encoder_worker.h"
#include "frame_hub.h"
#include "h264_encoder.h"
#include "log.h"
#include "mdns.h"
#include "source_worker.h"
#include "sysinfo.h"
#include "video_source.h"
#include "webrtc_session.h"

/* Generated at build time from web/index.html by tools/embed_assets. */
#include "web_assets.h"

#define MAX_RTC_SESSIONS 8
#define AU_RING_SLOTS 8
#define AU_SLOT_CAPACITY (512 * 1024)
#define MAX_DATAGRAMS_PER_POLL 32
#define RATE_WINDOW_MS 1000
#define JSON_STATUS_CAPACITY 8192
#define JSON_STATS_CAPACITY 16384
#define JSON_LOGS_CAPACITY 49152
#define MAX_INTERFACES 16

/* Poll set: HTTP connections, session sockets, mDNS, AU ring. */
#define MAX_HTTP_POLL_FDS 64
#define MAX_POLL_FDS (MAX_HTTP_POLL_FDS + MAX_RTC_SESSIONS + 2)

/* Main loop wakeups when nothing is pending. */
#define IDLE_WAKE_MS 100
#define SESSION_WAKE_MS 20

/* Automatic pipeline recovery: first retry, then doubling up to max. */
#define PIPELINE_RETRY_MS 5000
#define PIPELINE_RETRY_MAX_MS 60000

/* Layer health: how recent an event must be to count as "flowing". */
#define MEDIA_RECENT_MS 2000
#define RR_RECENT_MS 6000
#define CLIENT_REPORT_RECENT_MS 6000

/* Rejected TLS connections on the plain HTTP port. */
#define TLS_WARN_INTERVAL_MS 10000
#define TLS_LINGER_MS 1000

typedef struct {
    char name[IF_NAMESIZE + 1];
    char ip[INET_ADDRSTRLEN];
} InterfaceInfo;

typedef struct {
    /* Owned configuration copy: reloads mutate this, not the caller's. */
    AppConfig config;

    struct mg_mgr mgr;
    struct mg_connection *listener;
    mg_event_handler_t http_protocol;   /* mongoose's HTTP pfn, see tls_guard */
    char http_bound[80];                /* "0.0.0.0:8080" from getsockname */
    uint64_t tls_rejected;              /* TLS ClientHellos on the HTTP port */
    uint64_t tls_warned_ms;

    /* Media pipeline, rebuilt by media_pipeline_start/stop. */
    VideoSource *source;
    SourceWorker *source_worker;
    FrameHub *hub;
    H264Encoder *encoder;
    EncoderWorker *encoder_worker;
    AuRing *ring;
    char encoder_name[96];
    atomic_int force_idr;
    atomic_int media_active;        /* encoder actively producing */
    int pipeline_running;

    /*
     * Runtime recovery. A failed capture or encoder thread is detected
     * by media_supervise(), which tears the pipeline down and rebuilds
     * it with a backoff. When a hardware encoder dies and the operator
     * asked for "auto", the rebuild uses libx264 (encoder_override).
     */
    char encoder_override[8];
    char pipeline_error[256];
    uint64_t pipeline_retry_ms;     /* next rebuild attempt, 0 = none */
    unsigned pipeline_failures;     /* consecutive, drives the backoff */
    uint64_t pipeline_recoveries;
    uint64_t au_discontinuities;    /* AUs lost between encoder and sender */

    /*
     * mDNS responder, NULL when it is off or could not be created. It
     * only publishes a name; the HTTP listener above is what serves the
     * page, so a missing responder degrades to "type the address".
     */
    MdnsResponder *mdns;

    RtcSession *sessions[MAX_RTC_SESSIONS];
    uint64_t sessions_total;
    uint64_t session_create_failures;
    uint64_t sessions_closed;
    int dtls_ready;

    /* Rate sampling, refreshed once per second. */
    uint64_t sample_ms;
    uint64_t sample_bytes_sent;
    uint64_t sample_captured;
    uint64_t sample_encoded;
    double bitrate_kbps;
    double capture_fps;
    double encode_fps;
    uint64_t session_sample_ms[MAX_RTC_SESSIONS];
    uint64_t session_sample_bytes[MAX_RTC_SESSIONS];
    double session_bitrate_kbps[MAX_RTC_SESSIONS];

    /*
     * What the browser itself reports (POST /api/webrtc/client-stats):
     * the only layer the server cannot observe directly is decoding.
     */
    uint64_t client_frames_decoded[MAX_RTC_SESSIONS];
    uint64_t client_report_ms[MAX_RTC_SESSIONS];
    unsigned client_width[MAX_RTC_SESSIONS];
    unsigned client_height[MAX_RTC_SESSIONS];
    int client_decoding[MAX_RTC_SESSIONS];  /* count grew at the last report */

    uint64_t started_ms;
    uint64_t http_requests;
    uint64_t http_errors;
    uint64_t camera_restarts;
    uint64_t webrtc_restarts;
    uint64_t config_reloads;

    volatile sig_atomic_t *stop_flag;
} Server;

static uint64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t) ts.tv_sec * 1000ULL +
           (uint64_t) ts.tv_nsec / 1000000ULL;
}

/* ------------------------------------------------------------------ */
/* Network helpers                                                     */
/* ------------------------------------------------------------------ */

/*
 * Collect IPv4 addresses of interfaces that are up, excluding loopback.
 * Used both for the startup banner and to advertise host candidates.
 */
static size_t collect_interfaces(InterfaceInfo *out, size_t max)
{
    struct ifaddrs *addrs = NULL;

    if (getifaddrs(&addrs) != 0) {
        log_warn("net", "getifaddrs failed: errno=%d (%s)", errno,
                 strerror(errno));
        return 0;
    }

    size_t count = 0;

    for (struct ifaddrs *ifa = addrs;
         ifa != NULL && count < max;
         ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }

        if ((ifa->ifa_flags & IFF_UP) == 0) {
            continue;
        }

        const struct sockaddr_in *a = (const struct sockaddr_in *) ifa->ifa_addr;

        if (ntohl(a->sin_addr.s_addr) >> 24 == 127) {
            continue;
        }

        /* Skip duplicate addresses (aliases share one address). */
        char ip[INET_ADDRSTRLEN] = "";

        if (inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip)) == NULL) {
            continue;
        }

        int duplicate = 0;

        for (size_t i = 0; i < count; i++) {
            if (strcmp(out[i].ip, ip) == 0) {
                duplicate = 1;
                break;
            }
        }

        if (duplicate) {
            continue;
        }

        snprintf(out[count].name, sizeof(out[count].name), "%s", ifa->ifa_name);
        snprintf(out[count].ip, sizeof(out[count].ip), "%s", ip);
        count++;
    }

    freeifaddrs(addrs);

    return count;
}

static int is_private_ipv4(const char *ip)
{
    struct in_addr addr;

    if (inet_pton(AF_INET, ip, &addr) != 1) {
        return 0;
    }

    uint32_t host = ntohl(addr.s_addr);

    return (host >> 24) == 10 ||
           (host >> 20) == 0xAC1 ||         /* 172.16/12 */
           (host >> 16) == 0xC0A8 ||        /* 192.168/16 */
           (host >> 16) == 0xA9FE;          /* 169.254/16 link local */
}

/*
 * Pick the address to advertise in the SDP candidate and c= line.
 *
 * The best answer is the address the browser actually reached us on: a
 * LAN host with several interfaces (Ethernet, Wi-Fi, docker0) would
 * otherwise hand out the wrong one and the browser would send its checks
 * into nowhere. Falls back to the first private address, then to any
 * address, then to loopback for a local browser.
 */
static void choose_advertise_ip(const char *host_header,
                                const InterfaceInfo *interfaces,
                                size_t count,
                                char *out,
                                size_t out_size)
{
    if (host_header != NULL && host_header[0] != 0) {
        char candidate[INET_ADDRSTRLEN] = "";
        size_t i = 0;

        for (; host_header[i] != 0 && host_header[i] != ':' &&
               i + 1 < sizeof(candidate); i++) {
            candidate[i] = host_header[i];
        }
        candidate[i] = 0;

        for (size_t n = 0; n < count; n++) {
            if (strcmp(interfaces[n].ip, candidate) == 0) {
                snprintf(out, out_size, "%s", candidate);
                return;
            }
        }
    }

    for (size_t n = 0; n < count; n++) {
        if (is_private_ipv4(interfaces[n].ip)) {
            snprintf(out, out_size, "%s", interfaces[n].ip);
            return;
        }
    }

    if (count > 0) {
        snprintf(out, out_size, "%s", interfaces[0].ip);
        return;
    }

    snprintf(out, out_size, "127.0.0.1");
}

/* ------------------------------------------------------------------ */
/* JSON writer                                                         */
/* ------------------------------------------------------------------ */

/*
 * Small append-only JSON writer. Every append is bounds checked and a
 * short write latches 'truncated' instead of silently producing invalid
 * JSON: a truncated payload would otherwise be served as if complete.
 */
typedef struct {
    char *buffer;
    size_t capacity;
    size_t length;
    int truncated;
} JsonWriter;

static void json_begin(JsonWriter *writer, char *buffer, size_t capacity)
{
    writer->buffer = buffer;
    writer->capacity = capacity;
    writer->length = 0;
    writer->truncated = 0;

    if (capacity > 0) {
        buffer[0] = 0;
    }
}

static void json_raw(JsonWriter *writer, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

static void json_raw(JsonWriter *writer, const char *format, ...)
{
    if (writer->truncated || writer->length + 1 >= writer->capacity) {
        writer->truncated = 1;
        return;
    }

    va_list args;

    va_start(args, format);

    int written = vsnprintf(writer->buffer + writer->length,
                            writer->capacity - writer->length,
                            format, args);

    va_end(args);

    if (written < 0 ||
        (size_t) written >= writer->capacity - writer->length) {
        writer->truncated = 1;
        return;
    }

    writer->length += (size_t) written;
}

static void json_string(JsonWriter *writer, const char *value)
{
    json_raw(writer, "\"");

    for (const char *p = value; *p != 0; p++) {
        unsigned char c = (unsigned char) *p;

        if (c == '"' || c == '\\') {
            json_raw(writer, "\\%c", c);
        } else if (c == '\n') {
            json_raw(writer, "\\n");
        } else if (c == '\r') {
            json_raw(writer, "\\r");
        } else if (c == '\t') {
            json_raw(writer, "\\t");
        } else if (c < 0x20) {
            json_raw(writer, "?");
        } else {
            json_raw(writer, "%c", c);
        }
    }

    json_raw(writer, "\"");
}

static int json_finish(JsonWriter *writer)
{
    if (writer->truncated) {
        return -1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Media pipeline                                                      */
/* ------------------------------------------------------------------ */

static VideoSource *create_source(const AppConfig *config)
{
    switch (config->source_kind) {
    case SOURCE_TEST:
        return test_source_create(config->width, config->height, config->fps);

    case SOURCE_CSI:
        return csi_source_create(config->rpicam_bin[0] ? config->rpicam_bin
                                                       : NULL,
                                 config->width, config->height,
                                 config->fps, config->verbose);

    case SOURCE_STDIN:
        return stdin_source_create(config->width, config->height, config->fps);

    case SOURCE_V4L2:
    default: {
        VideoSource *vsrc = v4l2_source_create(config->device, config->width,
                                               config->height, config->fps);
        if (vsrc == NULL) {
            log_warn("capture", "V4L2 source failed on %s. If using a Raspberry Pi CSI camera (e.g. IMX219), switch to '-s csi' or 'source = csi'", config->device);
        }
        return vsrc;
    }
    }
}

/*
 * Defined below media_pipeline_start(), whose failure path calls it to
 * unwind a partially built pipeline. It only touches members that are
 * non-NULL, so it is safe at any point of the build.
 */
static void media_pipeline_stop(Server *server);

/*
 * Build the pipeline. On failure the caller gets a message that names
 * the subsystem and the setting that failed, and the partially built
 * pipeline is torn down again.
 */
static int media_pipeline_start(Server *server, char *error, size_t error_size)
{
    const AppConfig *config = &server->config;

    server->source = create_source(config);

    if (server->source == NULL) {
        snprintf(error, error_size,
                 "capture: cannot open source '%s' (see the log for the "
                 "driver or file error)", config->source_name);
        goto fail;
    }

    server->hub = frame_hub_create(server->source->frame_size, 6);

    if (server->hub == NULL) {
        snprintf(error, error_size,
                 "capture: frame pool allocation failed (%zu byte frames)",
                 server->source->frame_size);
        goto fail;
    }

    server->ring = au_ring_create(AU_SLOT_CAPACITY, AU_RING_SLOTS);

    if (server->ring == NULL) {
        snprintf(error, error_size,
                 "encode: access unit ring allocation failed (%d x %d KB)",
                 AU_RING_SLOTS, AU_SLOT_CAPACITY / 1024);
        goto fail;
    }

    const char *preference = server->encoder_override[0] != 0
                                 ? server->encoder_override
                                 : config->encoder;

    server->encoder = h264_encoder_open(preference,
                                        server->source->width,
                                        server->source->height,
                                        server->source->fps,
                                        config->bitrate_kbps,
                                        config->keyframe_seconds,
                                        server->encoder_name,
                                        sizeof(server->encoder_name));

    if (server->encoder == NULL) {
        snprintf(error, error_size,
                 "encode: no usable encoder for preference '%s'",
                 preference);
        goto fail;
    }

    server->encoder_worker = encoder_worker_create(server->hub,
                                                   server->encoder,
                                                   server->source->width,
                                                   server->source->height,
                                                   server->ring,
                                                   &server->force_idr,
                                                   &server->media_active);

    if (server->encoder_worker == NULL ||
        encoder_worker_start(server->encoder_worker) != 0) {
        snprintf(error, error_size,
                 "encode: worker thread did not start for %ux%u",
                 server->source->width, server->source->height);
        goto fail;
    }

    server->source_worker = source_worker_create(server->source, server->hub);

    if (server->source_worker == NULL ||
        source_worker_start(server->source_worker) != 0) {
        snprintf(error, error_size, "capture: worker thread did not start for %s",
                 server->source->name);
        goto fail;
    }

    server->pipeline_running = 1;
    atomic_store(&server->force_idr, 1);

    log_info("media", "pipeline running: capture %s %ux%u @ %u fps -> "
                      "%s encoder %s, keyframe every %u s",
             server->source->name,
             server->source->width, server->source->height,
             server->source->fps,
             h264_encoder_kind(server->encoder) == H264_ENCODER_HW
                 ? "hardware" : "software",
             server->encoder_name, config->keyframe_seconds);

    return 0;

fail:
    log_error("media", "pipeline start failed: %s", error);
    media_pipeline_stop(server);
    return -1;
}

/*
 * Tear the pipeline down in dependency order: stop the threads first
 * (the encoder thread reads the hub, the source thread writes it), then
 * release the objects they used. Safe to call on a partial pipeline and
 * twice in a row.
 */
static void media_pipeline_stop(Server *server)
{
    if (server->source_worker != NULL) {
        source_worker_stop(server->source_worker);
        source_worker_destroy(server->source_worker);
        server->source_worker = NULL;
    }

    if (server->encoder_worker != NULL) {
        encoder_worker_stop(server->encoder_worker);
        encoder_worker_destroy(server->encoder_worker);
        server->encoder_worker = NULL;
    }

    if (server->encoder != NULL) {
        h264_encoder_close(server->encoder);
        server->encoder = NULL;
    }

    if (server->ring != NULL) {
        au_ring_destroy(server->ring);
        server->ring = NULL;
    }

    if (server->hub != NULL) {
        frame_hub_destroy(server->hub);
        server->hub = NULL;
    }

    if (server->source != NULL) {
        video_source_close(server->source);
        server->source = NULL;
    }

    server->pipeline_running = 0;
    server->encoder_name[0] = 0;
    atomic_store(&server->media_active, 0);
}

/*
 * Schedule the next automatic rebuild: 5 s, 10 s, 20 s ... capped at a
 * minute, so a camera that is unplugged or held by another process does
 * not flood the log.
 */
static void pipeline_schedule_retry(Server *server, uint64_t now)
{
    uint64_t delay = PIPELINE_RETRY_MS;

    for (unsigned i = 1; i < server->pipeline_failures &&
                         delay < PIPELINE_RETRY_MAX_MS; i++) {
        delay *= 2;
    }

    if (delay > PIPELINE_RETRY_MAX_MS) {
        delay = PIPELINE_RETRY_MAX_MS;
    }

    server->pipeline_retry_ms = now + delay;

    log_warn("media", "next pipeline start attempt in %llu s",
             (unsigned long long) (delay / 1000));
}

/*
 * Runtime health of the capture and encode threads. Either one exiting
 * (camera unplugged, rpicam-vid died, encoder fatal error or stalled)
 * used to leave a pipeline that looked "running" but produced nothing;
 * now it is torn down and rebuilt, and the viewers get a keyframe from
 * the new encoder. Sessions survive the rebuild.
 */
static void media_supervise(Server *server, uint64_t now)
{
    if (server->pipeline_running) {
        int encoder_failed = encoder_worker_failed(server->encoder_worker);
        int source_failed = source_worker_failed(server->source_worker);

        if (!encoder_failed && !source_failed) {
            return;
        }

        if (encoder_failed) {
            int hardware =
                h264_encoder_kind(server->encoder) == H264_ENCODER_HW;

            snprintf(server->pipeline_error, sizeof(server->pipeline_error),
                     "encode: %s failed at runtime", server->encoder_name);

            if (hardware && strcmp(server->config.encoder, "auto") == 0) {
                snprintf(server->encoder_override,
                         sizeof(server->encoder_override), "sw");
                log_warn("media", "hardware encoder %s failed; rebuilding "
                                  "the pipeline with libx264 (encoder=auto)",
                         server->encoder_name);
            } else {
                log_error("media", "encoder %s failed; rebuilding the "
                                   "pipeline", server->encoder_name);
            }
        } else {
            snprintf(server->pipeline_error, sizeof(server->pipeline_error),
                     "capture: %s stopped delivering frames",
                     server->source->name);
            log_error("media", "capture %s stopped; rebuilding the pipeline",
                      server->source->name);
        }

        media_pipeline_stop(server);

        /* An encoder switch is worth trying at once; a camera is not. */
        server->pipeline_failures = encoder_failed ? 0 : 1;
        server->pipeline_retry_ms =
            encoder_failed ? now : now + PIPELINE_RETRY_MS;
        return;
    }

    if (server->pipeline_retry_ms == 0 || now < server->pipeline_retry_ms) {
        return;
    }

    char error[256] = "";

    if (media_pipeline_start(server, error, sizeof(error)) == 0) {
        server->pipeline_retry_ms = 0;
        server->pipeline_failures = 0;
        server->pipeline_error[0] = 0;
        server->pipeline_recoveries++;
        log_info("media", "pipeline recovered");
        return;
    }

    snprintf(server->pipeline_error, sizeof(server->pipeline_error), "%s",
             error);
    server->pipeline_failures++;
    pipeline_schedule_retry(server, now);
}

/* ------------------------------------------------------------------ */
/* Sessions                                                            */
/* ------------------------------------------------------------------ */

static void session_on_idr_request(void *user)
{
    Server *server = user;

    atomic_store(&server->force_idr, 1);
}

static void session_on_closed(void *user, RtcSession *session)
{
    Server *server = user;

    server->sessions_closed++;

    log_info("rtc", "%08x: closed", rtc_session_id(session));
}

static void sessions_close_all(Server *server)
{
    for (size_t i = 0; i < MAX_RTC_SESSIONS; i++) {
        if (server->sessions[i] != NULL) {
            /* rtc_session_destroy() closes it first when still open. */
            rtc_session_destroy(server->sessions[i]);
            server->sessions[i] = NULL;
        }
    }

    atomic_store(&server->media_active, 0);
}

static int sessions_active(const Server *server)
{
    int active = 0;

    for (size_t i = 0; i < MAX_RTC_SESSIONS; i++) {
        if (server->sessions[i] != NULL &&
            rtc_session_state(server->sessions[i]) != RTC_CLOSED) {
            active++;
        }
    }

    return active;
}

/*
 * A session id is a handle the browser uses to close its own session,
 * so it must not be guessable: a predicted id would let any LAN host
 * tear down another viewer. Uniqueness among live sessions is checked
 * because the UI shows the value.
 */
static uint32_t next_session_id(const Server *server)
{
    for (int attempt = 0; attempt < 16; attempt++) {
        uint32_t candidate = 0;

        if (RAND_bytes((unsigned char *) &candidate, sizeof(candidate)) != 1) {
            break;
        }

        if (candidate == 0) {
            continue;
        }

        int used = 0;

        for (size_t i = 0; i < MAX_RTC_SESSIONS; i++) {
            if (server->sessions[i] != NULL &&
                rtc_session_id(server->sessions[i]) == candidate) {
                used = 1;
                break;
            }
        }

        if (!used) {
            return candidate;
        }
    }

    log_error("http", "RAND_bytes failed for session ids; using a counter");

    return (uint32_t) (server->sessions_total + 1) * 2654435761u;
}

/* ------------------------------------------------------------------ */
/* HTTP transport                                                      */
/* ------------------------------------------------------------------ */

/* "192.168.0.20:53124" / "[fe80::1]:53124" for logs and diagnostics. */
static void format_mg_addr(const struct mg_addr *addr, char *out, size_t size)
{
    char ip[INET6_ADDRSTRLEN] = "?";

    if (addr->is_ip6) {
        inet_ntop(AF_INET6, addr->addr.ip, ip, sizeof(ip));
        snprintf(out, size, "[%s]:%u", ip, (unsigned) ntohs(addr->port));
    } else {
        inet_ntop(AF_INET, addr->addr.ip, ip, sizeof(ip));
        snprintf(out, size, "%s:%u", ip, (unsigned) ntohs(addr->port));
    }
}

/*
 * Mongoose reports a failed listen only as a NULL return (its own log
 * is compiled out), which used to surface as "cannot listen" with no
 * reason. Bind a throwaway socket with the same options first so the
 * operator gets the errno and the usual fix. Only IPv4 literals are
 * probed; anything else is left to mongoose.
 */
static int http_probe_bind(const char *listen_ip, uint16_t port,
                           char *error, size_t error_size)
{
    struct sockaddr_in address;

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);

    if (inet_pton(AF_INET, listen_ip, &address.sin_addr) != 1) {
        return 0;
    }

    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (fd < 0) {
        snprintf(error, error_size, "socket() failed: %s", strerror(errno));
        return -1;
    }

    int one = 1;

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    int result = bind(fd, (struct sockaddr *) &address, sizeof(address));
    int saved = errno;

    close(fd);

    if (result == 0) {
        return 0;
    }

    switch (saved) {
    case EADDRINUSE:
        snprintf(error, error_size,
                 "TCP port %u is already in use (camstream service "
                 "running?). Check: sudo ss -ltnp 'sport = :%u'. Stop it: "
                 "sudo systemctl stop camstream",
                 (unsigned) port, (unsigned) port);
        break;
    case EACCES:
        snprintf(error, error_size,
                 "permission denied for TCP port %u (ports below 1024 need "
                 "root or CAP_NET_BIND_SERVICE)", (unsigned) port);
        break;
    case EADDRNOTAVAIL:
        snprintf(error, error_size,
                 "%s is not an address of this machine; use 0.0.0.0 to "
                 "listen on every interface", listen_ip);
        break;
    default:
        snprintf(error, error_size, "bind %s:%u failed: %s", listen_ip,
                 (unsigned) port, strerror(saved));
        break;
    }

    return -1;
}

/* Mongoose's hexdump and error output go through this: keep it quiet. */
static void mongoose_log_discard(char c, void *param)
{
    (void) c;
    (void) param;
}

/*
 * TLS on the plain HTTP port.
 *
 * Browsers upgrade typed addresses to https:// (HTTPS-First / HTTPS-Only
 * modes, HSTS for a name, or a bookmarked https:// URL). The first bytes
 * are then a TLS ClientHello (record type 0x16, version 0x03 0xNN), which
 * mongoose used to hexdump to stdout before closing the socket with the
 * rest of the hello unread. The kernel answers that with a RST, so the
 * browser showed PR_END_OF_FILE_ERROR or "connection was reset".
 *
 * This protocol filter runs before mongoose's HTTP parser on every
 * accepted connection. A TLS hello gets a TLS alert (protocol_version),
 * which browsers turn into a clear "can't connect securely" page, then
 * an orderly shutdown after the peer's data is drained, and one
 * rate-limited log line telling the operator to use http://.
 *
 * c->data[0]: 0 = undecided, 1 = HTTP, 2 = TLS rejected.
 * c->data[8..15]: monotonic ms of the rejection (for the linger bound).
 */
enum { CONN_UNDECIDED = 0, CONN_HTTP = 1, CONN_TLS_REJECTED = 2 };

static void tls_reject(Server *server, struct mg_connection *connection)
{
    static const uint8_t alert[] = {
        0x15, 0x03, 0x01, 0x00, 0x02,   /* alert record, TLS 1.0 framing */
        0x02, 0x46                      /* fatal, protocol_version */
    };
    uint64_t now = now_ms();

    connection->data[0] = CONN_TLS_REJECTED;
    memcpy(connection->data + 8, &now, sizeof(now));

    connection->recv.len = 0;
    mg_send(connection, alert, sizeof(alert));

    server->tls_rejected++;

    if (server->tls_warned_ms == 0 ||
        now - server->tls_warned_ms >= TLS_WARN_INTERVAL_MS) {
        char peer[64];

        format_mg_addr(&connection->rem, peer, sizeof(peer));
        server->tls_warned_ms = now;
        log_warn("http", "%s: TLS handshake on the plain HTTP port, the "
                         "browser is using https://. Open http://<pi-ip>:%u/ "
                         "(type http:// explicitly; Firefox HTTPS-Only needs "
                         "an exception) [%llu rejected]",
                 peer, (unsigned) server->config.http_port,
                 (unsigned long long) server->tls_rejected);
    }
}

static void tls_guard(struct mg_connection *connection, int event,
                      void *event_data)
{
    Server *server = connection->fn_data;

    if (connection->data[0] == CONN_TLS_REJECTED) {
        if (event == MG_EV_READ) {
            connection->recv.len = 0;         /* drain, never parse */
        } else if (event == MG_EV_POLL && event_data != NULL) {
            uint64_t since = 0;

            memcpy(&since, connection->data + 8, sizeof(since));

            if (connection->send.len == 0 && connection->data[1] == 0) {
                /* Alert flushed: FIN our side, keep reading theirs. */
                shutdown((int) (size_t) connection->fd, SHUT_WR);
                connection->data[1] = 1;
            }

            if (now_ms() - since >= TLS_LINGER_MS) {
                connection->is_closing = 1;
            }
        }
        return;
    }

    if (connection->data[0] == CONN_UNDECIDED && event == MG_EV_READ &&
        connection->recv.len > 0) {
        const uint8_t *head = connection->recv.buf;

        if (head[0] == 0x16 &&
            (connection->recv.len < 2 || head[1] == 0x03)) {
            tls_reject(server, connection);
            return;
        }

        connection->data[0] = CONN_HTTP;
    }

    if (server->http_protocol != NULL) {
        server->http_protocol(connection, event, event_data);
    }
}

/* ------------------------------------------------------------------ */
/* HTTP handlers                                                       */
/* ------------------------------------------------------------------ */

static int uri_is(const struct mg_str *uri, const char *path)
{
    size_t length = strlen(path);

    if (uri->len < length || memcmp(uri->buf, path, length) != 0) {
        return 0;
    }

    if (uri->len == length) {
        return 1;
    }

    return uri->buf[length] == '?' || uri->buf[length] == '#';
}

static void reply_json(Server *server,
                       struct mg_connection *connection,
                       int status,
                       const char *body)
{
    if (status >= 400) {
        server->http_errors++;
    }

    mg_http_reply(connection, status,
                  "Content-Type: application/json\r\nCache-Control: no-store\r\n",
                  "%s", body);
}

static void reply_error(Server *server,
                        struct mg_connection *connection,
                        int status,
                        const char *message)
{
    char buffer[512];
    JsonWriter writer;

    json_begin(&writer, buffer, sizeof(buffer));
    json_raw(&writer, "{\"error\":");
    json_string(&writer, message);
    json_raw(&writer, "}");

    if (json_finish(&writer) != 0) {
        mg_http_reply(connection, 500,
                      "Content-Type: application/json\r\n",
                      "{\"error\":\"error reply overflow\"}");
        server->http_errors++;
        return;
    }

    log_warn("http", "request failed: %d %s", status, message);
    reply_json(server, connection, status, buffer);
}

/*
 * Copy a JSON string field out of a request body. The body is trusted
 * only after this: no path, length or index from the browser is used
 * without bounds, and the extractor refuses anything that would not fit.
 */
static int json_get_string(const char *body, const char *field,
                           char *out, size_t out_size)
{
    char pattern[64];

    if (snprintf(pattern, sizeof(pattern), "\"%s\"", field) >=
        (int) sizeof(pattern)) {
        return -1;
    }

    const char *pos = body;

    while ((pos = strstr(pos, pattern)) != NULL) {
        const char *p = pos + strlen(pattern);

        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            p++;
        }

        if (*p != ':') {
            pos++;
            continue;
        }

        p++;

        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            p++;
        }

        if (*p != '"') {
            return -1;
        }

        p++;

        size_t written = 0;

        while (*p != 0 && *p != '"') {
            char decoded;

            if (*p == '\\') {
                p++;

                switch (*p) {
                case 'n': decoded = '\n'; break;
                case 'r': decoded = '\r'; break;
                case 't': decoded = '\t'; break;
                case '"': decoded = '"'; break;
                case '\\': decoded = '\\'; break;
                case '/': decoded = '/'; break;
                case 'u': {
                    /* Only the BMP subset that fits ASCII is kept. */
                    char hex[5] = { 0, 0, 0, 0, 0 };

                    for (int i = 0; i < 4 && p[1] != 0; i++) {
                        hex[i] = *++p;
                    }

                    long code = strtol(hex, NULL, 16);

                    decoded = code > 0 && code < 128 ? (char) code : '?';
                    break;
                }
                default:
                    decoded = *p;
                    break;
                }
            } else {
                decoded = *p;
            }

            if (written + 1 >= out_size) {
                return -1;
            }

            out[written++] = decoded;
            p++;
        }

        if (*p != '"') {
            return -1;
        }

        out[written] = 0;

        return *p == '"' ? 0 : -1;
    }

    return -1;
}

/*
 * 64-bit on purpose: session ids are random uint32_t values and "long"
 * is 32 bits on armhf Raspberry Pi OS, where ids above 2^31 used to
 * saturate and "no such session" came back for half of all viewers.
 */
static int json_get_long(const char *body, const char *field, long long *out)
{
    char pattern[64];

    if (snprintf(pattern, sizeof(pattern), "\"%s\"", field) >=
        (int) sizeof(pattern)) {
        return -1;
    }

    const char *pos = strstr(body, pattern);

    if (pos == NULL) {
        return -1;
    }

    pos += strlen(pattern);

    while (*pos == ' ' || *pos == '\t' || *pos == '\r' || *pos == '\n') {
        pos++;
    }

    if (*pos != ':') {
        return -1;
    }

    pos++;

    char *end = NULL;

    errno = 0;

    long long value = strtoll(pos, &end, 10);

    if (end == pos || errno == ERANGE) {
        return -1;
    }

    *out = value;

    return 0;
}

static void handle_offer(Server *server,
                         struct mg_connection *connection,
                         struct mg_http_message *message)
{
    char body[16384];

    if (message->body.len == 0 || message->body.len >= sizeof(body)) {
        reply_error(server, connection, 413,
                    "signaling request body must be 1 to 16383 bytes");
        return;
    }

    memcpy(body, message->body.buf, message->body.len);
    body[message->body.len] = 0;

    char sdp[8192];

    if (json_get_string(body, "sdp", sdp, sizeof(sdp)) != 0) {
        reply_error(server, connection, 400,
                    "missing or oversized 'sdp' string field");
        return;
    }

    SdpOffer offer;
    char reason[192] = "";

    if (sdp_parse_offer(sdp, strlen(sdp), &offer, reason,
                        sizeof(reason)) != 0) {
        reply_error(server, connection, 400, reason);
        return;
    }

    if (!server->dtls_ready) {
        reply_error(server, connection, 503,
                    "WebRTC unavailable: DTLS is not initialised, see "
                    "/api/logs and POST /api/webrtc/restart");
        return;
    }

    size_t slot = MAX_RTC_SESSIONS;

    for (size_t i = 0; i < MAX_RTC_SESSIONS; i++) {
        if (server->sessions[i] == NULL) {
            slot = i;
            break;
        }
    }

    if (slot == MAX_RTC_SESSIONS) {
        reply_error(server, connection, 503,
                    "session limit reached (8 viewers); close a viewer or "
                    "use POST /api/webrtc/close");
        return;
    }

    InterfaceInfo interfaces[MAX_INTERFACES];
    size_t interface_count = collect_interfaces(interfaces, MAX_INTERFACES);

    char host_header[256] = "";
    const struct mg_str *host = mg_http_get_header(message, "Host");

    if (host != NULL && host->len < sizeof(host_header)) {
        memcpy(host_header, host->buf, host->len);
        host_header[host->len] = 0;
    }

    RtcSessionConfig session_config;

    memset(&session_config, 0, sizeof(session_config));

    char advertise_ip[INET_ADDRSTRLEN];

    choose_advertise_ip(host_header, interfaces, interface_count,
                        advertise_ip, sizeof(advertise_ip));

    snprintf(session_config.candidate_ips[0],
             sizeof(session_config.candidate_ips[0]), "%s", advertise_ip);
    session_config.candidate_count = 1;

    /*
     * Add the other local addresses as extra host candidates: a
     * multi-homed host (Ethernet plus Wi-Fi plus a VPN interface) can
     * then still be reached when the browser picks a different path
     * than the one used for HTTP.
     */
    for (size_t i = 0; i < interface_count &&
                       session_config.candidate_count < SDP_MAX_CANDIDATES;
         i++) {
        if (strcmp(interfaces[i].ip, advertise_ip) == 0) {
            continue;
        }

        int duplicate = 0;

        for (size_t n = 0; n < session_config.candidate_count; n++) {
            if (strcmp(session_config.candidate_ips[n], interfaces[i].ip) == 0) {
                duplicate = 1;
                break;
            }
        }

        if (duplicate) {
            continue;
        }

        snprintf(session_config.candidate_ips[session_config.candidate_count],
                 sizeof(session_config.candidate_ips[0]), "%s",
                 interfaces[i].ip);
        session_config.candidate_count++;
    }

    format_mg_addr(&connection->rem, session_config.signaling_peer,
                   sizeof(session_config.signaling_peer));

    session_config.id = next_session_id(server);
    session_config.udp_port = (uint16_t) (server->config.udp_base_port + slot);
    session_config.offer = offer;
    session_config.server = server;
    session_config.on_idr_request = session_on_idr_request;
    session_config.on_closed = session_on_closed;

    RtcSession *session = NULL;
    char answer[8192];
    size_t answer_length = 0;

    if (rtc_session_create(&session_config, &session, answer,
                           sizeof(answer), &answer_length) != 0) {
        server->session_create_failures++;
        reply_error(server, connection, 503,
                    "WebRTC session setup failed (UDP port busy or DTLS "
                    "unavailable, see /api/logs)");
        return;
    }

    server->sessions[slot] = session;
    server->sessions_total++;
    server->session_sample_ms[slot] = 0;
    server->session_sample_bytes[slot] = 0;
    server->session_bitrate_kbps[slot] = 0.0;
    server->client_frames_decoded[slot] = 0;
    server->client_report_ms[slot] = 0;
    server->client_width[slot] = 0;
    server->client_height[slot] = 0;
    server->client_decoding[slot] = 0;

    /* A viewer joining mid-GOP needs a keyframe before it can decode. */
    atomic_store(&server->force_idr, 1);

    char payload[12288];
    JsonWriter writer;

    json_begin(&writer, payload, sizeof(payload));
    json_raw(&writer, "{\"type\":\"answer\",\"session_id\":%u,"
                      "\"udp_port\":%u,\"sdp\":",
             rtc_session_id(session), (unsigned) session_config.udp_port);
    json_string(&writer, answer);
    json_raw(&writer, "}");

    if (json_finish(&writer) != 0) {
        log_error("http", "SDP answer JSON overflow for session %08x",
                  rtc_session_id(session));
        rtc_session_destroy(session);
        server->sessions[slot] = NULL;
        server->sessions_total--;
        reply_error(server, connection, 500, "answer payload overflow");
        return;
    }

    reply_json(server, connection, 200, payload);

    log_info("rtc", "%08x: signaling complete for %s (slot %zu, UDP %u, "
                    "candidate %s + %zu more)",
             rtc_session_id(session), session_config.signaling_peer, slot,
             (unsigned) session_config.udp_port, advertise_ip,
             session_config.candidate_count - 1);
}

static void handle_close(Server *server,
                         struct mg_connection *connection,
                         struct mg_http_message *message)
{
    char body[512];

    if (message->body.len == 0 || message->body.len >= sizeof(body)) {
        reply_error(server, connection, 400,
                    "close request body must be 1 to 511 bytes");
        return;
    }

    memcpy(body, message->body.buf, message->body.len);
    body[message->body.len] = 0;

    long long id = 0;

    if (json_get_long(body, "session_id", &id) != 0 || id <= 0 ||
        id > UINT32_MAX) {
        reply_error(server, connection, 400,
                    "missing or invalid 'session_id'");
        return;
    }

    for (size_t i = 0; i < MAX_RTC_SESSIONS; i++) {
        if (server->sessions[i] != NULL &&
            rtc_session_id(server->sessions[i]) == (uint32_t) id) {
            rtc_session_close(server->sessions[i]);
            reply_json(server, connection, 200, "{\"closed\":true}");
            return;
        }
    }

    reply_error(server, connection, 404, "no such session");
}

/*
 * POST /api/webrtc/client-stats {"session_id":N,"frames_decoded":N,
 * "frame_width":N,"frame_height":N}
 *
 * The page posts what its RTCPeerConnection reports every couple of
 * seconds. Decoding happens in the browser, so without this the server
 * can prove packets left and were acknowledged (RTCP RR) but not that a
 * picture appeared. Purely diagnostic: nothing depends on it.
 */
static void handle_client_stats(Server *server,
                                struct mg_connection *connection,
                                struct mg_http_message *message)
{
    char body[512];

    if (message->body.len == 0 || message->body.len >= sizeof(body)) {
        reply_error(server, connection, 400,
                    "client stats body must be 1 to 511 bytes");
        return;
    }

    memcpy(body, message->body.buf, message->body.len);
    body[message->body.len] = 0;

    long long id = 0;
    long long decoded = 0;
    long long width = 0;
    long long height = 0;

    if (json_get_long(body, "session_id", &id) != 0 || id <= 0 ||
        id > UINT32_MAX ||
        json_get_long(body, "frames_decoded", &decoded) != 0 || decoded < 0) {
        reply_error(server, connection, 400,
                    "need 'session_id' and 'frames_decoded'");
        return;
    }

    if (json_get_long(body, "frame_width", &width) != 0 || width < 0 ||
        width > 16384) {
        width = 0;
    }

    if (json_get_long(body, "frame_height", &height) != 0 || height < 0 ||
        height > 16384) {
        height = 0;
    }

    for (size_t i = 0; i < MAX_RTC_SESSIONS; i++) {
        RtcSession *session = server->sessions[i];

        if (session == NULL || rtc_session_id(session) != (uint32_t) id) {
            continue;
        }

        server->client_decoding[i] =
            (uint64_t) decoded > server->client_frames_decoded[i];

        if (server->client_frames_decoded[i] == 0 && decoded > 0) {
            log_info("rtc", "%08x: browser is decoding video (%lldx%lld)",
                     rtc_session_id(session), width, height);
        }

        server->client_frames_decoded[i] = (uint64_t) decoded;
        server->client_width[i] = (unsigned) width;
        server->client_height[i] = (unsigned) height;
        server->client_report_ms[i] = now_ms();

        reply_json(server, connection, 200, "{\"ok\":true}");
        return;
    }

    reply_error(server, connection, 404, "no such session");
}

/*
 * Per-layer view over all live sessions. Each counter answers one
 * question in order, so a viewer with a black picture can be placed on
 * the first layer that is not flowing:
 *
 *   signaling  offer answered (the session exists)
 *   ice        an authenticated STUN check arrived from the browser
 *   dtls       the handshake completed and SRTP keys exist
 *   rtp        access units were packetized and sent recently
 *   rtcp       the browser acknowledges them with receiver reports
 *   decode     the page reports a growing framesDecoded count
 */
typedef struct {
    int sessions;
    int ice;
    int dtls;
    int rtp;
    int rtcp;
    int decode;
    int waiting_keyframe;
} LayerSummary;

static void layer_summary(const Server *server, uint64_t now,
                          LayerSummary *out)
{
    memset(out, 0, sizeof(*out));

    for (size_t i = 0; i < MAX_RTC_SESSIONS; i++) {
        const RtcSession *session = server->sessions[i];

        if (session == NULL || rtc_session_state(session) == RTC_CLOSED) {
            continue;
        }

        RtcSessionStats stats;

        memset(&stats, 0, sizeof(stats));
        rtc_session_get_stats(session, &stats);

        out->sessions++;
        out->ice += stats.stun_ok > 0;
        out->dtls += rtc_session_state(session) == RTC_STREAMING;
        out->rtp += stats.last_media_ms != 0 &&
                    now - stats.last_media_ms < MEDIA_RECENT_MS;
        out->rtcp += stats.last_rr_ms != 0 &&
                     now - stats.last_rr_ms < RR_RECENT_MS;
        out->decode += server->client_decoding[i] &&
                       now - server->client_report_ms[i] <
                           CLIENT_REPORT_RECENT_MS;
        out->waiting_keyframe += stats.waiting_keyframe &&
                                 rtc_session_state(session) == RTC_STREAMING;
    }
}

/*
 * Top level state. "streaming" means media is actually flowing: an
 * access unit went out in the last 2 s and the browser acknowledged the
 * stream with a receiver report, not merely that a session exists.
 */
static const char *server_state(const Server *server)
{
    if (!server->pipeline_running) {
        return "camera-error";
    }

    LayerSummary layers;

    layer_summary(server, now_ms(), &layers);

    if (layers.sessions == 0) {
        return "idle";
    }

    if (layers.rtp > 0 && layers.rtcp > 0) {
        return "streaming";
    }

    if (layers.rtp > 0) {
        return "sending";           /* no receiver report yet */
    }

    if (layers.dtls > 0) {
        return "no-media";          /* connected, nothing to send */
    }

    return "connecting";
}

/*
 * Most recent ERROR level message, for the top level health field of
 * /api/status. Extracted from the log module's JSON form so the log
 * stays the single source of truth for recent failures.
 */
static void latest_error(char *out, size_t out_size)
{
    char json[512];

    out[0] = 0;

    if (log_level_count(LOG_LEVEL_ERROR) == 0) {
        return;
    }

    if (log_recent_json(json, sizeof(json), 1, LOG_LEVEL_ERROR) == 0) {
        return;
    }

    const char *key = strstr(json, "\"msg\":\"");

    if (key == NULL) {
        return;
    }

    key += strlen("\"msg\":\"");

    size_t i = 0;

    while (key[i] != 0 && key[i] != '"' && i + 1 < out_size) {
        out[i] = key[i];
        i++;
    }

    out[i] = 0;
}

static void handle_status(Server *server,
                          struct mg_connection *connection)
{
    char buffer[JSON_STATUS_CAPACITY];
    JsonWriter writer;

    json_begin(&writer, buffer, sizeof(buffer));

    InterfaceInfo interfaces[MAX_INTERFACES];
    size_t interface_count = collect_interfaces(interfaces, MAX_INTERFACES);

    EncoderWorkerStats encoder_stats;

    memset(&encoder_stats, 0, sizeof(encoder_stats));

    if (server->encoder_worker != NULL) {
        encoder_worker_get_stats(server->encoder_worker, &encoder_stats);
    }

    DtlsSrtpGlobalStats dtls_stats;

    dtls_srtp_global_stats(&dtls_stats);

    json_raw(&writer, "{\"version\":\"%s\",\"uptime_sec\":%llu,\"state\":",
             APP_VERSION,
             (unsigned long long) ((now_ms() - server->started_ms) / 1000));
    json_string(&writer, server_state(server));

    unsigned http_connections = 0;

    for (struct mg_connection *c = server->mgr.conns; c != NULL; c = c->next) {
        http_connections += c->is_accepted;
    }

    json_raw(&writer, ",\"http\":{\"listen\":");
    json_string(&writer, server->http_bound);
    json_raw(&writer, ",\"port\":%u,\"connections\":%u,\"requests\":%llu,"
                      "\"errors\":%llu,\"tls_rejected\":%llu}",
             server->config.http_port, http_connections,
             (unsigned long long) server->http_requests,
             (unsigned long long) server->http_errors,
             (unsigned long long) server->tls_rejected);

    if (server->source != NULL) {
        const VideoSource *source = server->source;

        json_raw(&writer, ",\"source\":{\"kind\":");
        json_string(&writer, server->config.source_name);
        json_raw(&writer, ",\"name\":");
        json_string(&writer, source->name);
        json_raw(&writer, ",\"width\":%u,\"height\":%u,\"fps\":%u,"
                          "\"capture_fps\":%.1f,"
                          "\"frames\":%llu,\"capture_errors\":%llu,"
                          "\"status\":\"%s\"}",
                 source->width, source->height, source->fps,
                 server->capture_fps,
                 (unsigned long long) source_worker_captured(server->source_worker),
                 (unsigned long long) source_worker_errors(server->source_worker),
                 source_worker_failed(server->source_worker) ? "failed" : "ok");
    } else {
        json_raw(&writer, ",\"source\":{\"kind\":");
        json_string(&writer, server->config.source_name);
        json_raw(&writer, ",\"status\":\"failed\",\"capture_errors\":0}");
    }

    json_raw(&writer, ",\"encoder\":{\"name\":");
    json_string(&writer, server->encoder_name[0] ? server->encoder_name : "none");
    json_raw(&writer, ",\"preference\":");
    json_string(&writer, server->config.encoder);
    const char *encoder_status = "failed";

    if (server->pipeline_running) {
        if (encoder_worker_failed(server->encoder_worker)) {
            encoder_status = "failed";
        } else if (!atomic_load(&server->media_active)) {
            encoder_status = "idle";        /* no viewer: nothing encoded */
        } else {
            encoder_status = server->encode_fps > 0.0 ? "ok" : "stalled";
        }
    }

    json_raw(&writer, ",\"kind\":\"%s\"",
             server->encoder == NULL ? "none"
             : h264_encoder_kind(server->encoder) == H264_ENCODER_HW
                 ? "hardware" : "software");
    json_raw(&writer, ",\"bitrate_kbps\":%u,\"keyframe_seconds\":%u,"
                      "\"frames\":%llu,\"keyframes\":%llu,"
                      "\"params_prepended\":%llu,\"fps\":%.1f,"
                      "\"status\":\"%s\"}",
             server->config.bitrate_kbps,
             server->config.keyframe_seconds,
             (unsigned long long) encoder_stats.frames_encoded,
             (unsigned long long) encoder_stats.keyframes,
             (unsigned long long) encoder_stats.params_prepended,
             server->encode_fps, encoder_status);

    uint64_t now = now_ms();

    json_raw(&writer, ",\"pipeline\":{\"running\":%s,\"recoveries\":%llu,"
                      "\"retry_in_sec\":%llu,\"error\":",
             server->pipeline_running ? "true" : "false",
             (unsigned long long) server->pipeline_recoveries,
             (unsigned long long) (server->pipeline_retry_ms > now
                 ? (server->pipeline_retry_ms - now + 999) / 1000 : 0));
    json_string(&writer, server->pipeline_error);
    json_raw(&writer, "}");

    /*
     * One line per layer, bottom-up, so "where does it stop" can be read
     * without correlating counters: the first entry that is not "ok"
     * (or "idle") is the layer to debug.
     */
    LayerSummary layers;

    layer_summary(server, now, &layers);

    const char *capture_layer =
        server->source == NULL ? "failed"
        : source_worker_failed(server->source_worker) ? "failed"
        : server->capture_fps > 0.0 ? "ok" : "no-frames";

    json_raw(&writer, ",\"layers\":{\"capture\":\"%s\",\"encoder\":\"%s\","
                      "\"http\":\"ok\",\"sessions\":%d,\"ice\":%d,"
                      "\"dtls\":%d,\"rtp\":%d,\"rtcp\":%d,"
                      "\"browser_decode\":%d,\"waiting_keyframe\":%d}",
             capture_layer, encoder_status, layers.sessions, layers.ice,
             layers.dtls, layers.rtp, layers.rtcp, layers.decode,
             layers.waiting_keyframe);

    /*
     * The fingerprint is part of the diagnostics because it is the one
     * value an operator compares against the browser side (for example
     * chrome://webrtc-internals) when a handshake fails.
     */
    json_raw(&writer, ",\"webrtc\":{\"transport\":\"webrtc-native\","
                      "\"dtls\":\"%s\",\"fingerprint\":",
             server->dtls_ready ? "ready" : "failed");
    json_string(&writer, server->dtls_ready
                             ? dtls_srtp_local_fingerprint() : "none");
    json_raw(&writer, ",\"sessions_active\":%d,"
                      "\"sessions_total\":%llu,\"sessions_max\":%d,"
                      "\"session_create_failures\":%llu,"
                      "\"handshakes_started\":%llu,"
                      "\"handshakes_completed\":%llu,"
                      "\"handshake_failures\":%llu,"
                      "\"udp_base_port\":%u}",
             sessions_active(server),
             (unsigned long long) server->sessions_total,
             MAX_RTC_SESSIONS,
             (unsigned long long) server->session_create_failures,
             (unsigned long long) dtls_stats.handshakes_started,
             (unsigned long long) dtls_stats.handshakes_completed,
             (unsigned long long) dtls_stats.handshake_failures,
             server->config.udp_base_port);

    json_raw(&writer, ",\"mdns\":{\"enabled\":%s,\"name\":",
             server->mdns != NULL ? "true" : "false");

    if (server->mdns != NULL) {
        MdnsStats mdns_stats;

        mdns_responder_get_stats(server->mdns, &mdns_stats);

        json_string(&writer, mdns_stats.name);
        json_raw(&writer, ",\"port\":%u,\"url\":",
                 (unsigned) server->config.mdns_port);
        char mdns_url[MDNS_NAME_MAX + 16];

        snprintf(mdns_url, sizeof(mdns_url), "http://%s:%u/",
                 mdns_stats.name, server->config.http_port);
        json_string(&writer, mdns_url);
        json_raw(&writer, ",\"addresses\":[");
        for (size_t i = 0; i < mdns_stats.address_count; i++) {
            json_raw(&writer, "%s", i ? "," : "");
            json_string(&writer, mdns_stats.addresses[i]);
        }
        json_raw(&writer, "],\"queries\":%llu,\"responses\":%llu,"
                          "\"announcements\":%llu,\"conflicts\":%llu,"
                          "\"parse_errors\":%llu,\"socket_errors\":%llu,"
                          "\"renamed\":%s,\"disabled\":%s}",
                 (unsigned long long) mdns_stats.queries,
                 (unsigned long long) mdns_stats.responses,
                 (unsigned long long) mdns_stats.announcements,
                 (unsigned long long) mdns_stats.conflicts,
                 (unsigned long long) mdns_stats.parse_errors,
                 (unsigned long long) mdns_stats.socket_errors,
                 mdns_stats.renamed ? "true" : "false",
                 mdns_stats.disabled ? "true" : "false");
    } else {
        json_raw(&writer, "null,\"port\":%u}",
                 (unsigned) server->config.mdns_port);
    }

    json_raw(&writer, ",\"media\":{\"bitrate_kbps\":%.1f,\"capture_fps\":%.1f,"
                      "\"encode_fps\":%.1f,\"frames_seen\":%llu,"
                      "\"frames_encoded\":%llu,\"au_dropped\":%llu,"
                      "\"skipped_idle\":%llu,\"skipped_size\":%llu,"
                      "\"skipped_mismatch\":%llu,\"encoder_no_output\":%llu,"
                      "\"au_discontinuities\":%llu}",
             server->bitrate_kbps, server->capture_fps, server->encode_fps,
             (unsigned long long) encoder_stats.frames_seen,
             (unsigned long long) encoder_stats.frames_encoded,
             server->ring != NULL ?
                 (unsigned long long) au_ring_dropped(server->ring) : 0,
             (unsigned long long) encoder_stats.skipped_idle,
             (unsigned long long) encoder_stats.skipped_bad_size,
             (unsigned long long) encoder_stats.skipped_mismatch,
             (unsigned long long) encoder_stats.no_output,
             (unsigned long long) server->au_discontinuities);

    json_raw(&writer, ",\"process\":{\"cpu_percent\":%.1f,\"rss_kb\":%llu,"
                      "\"log_lines\":%llu,\"log_errors\":%llu,"
                      "\"log_warnings\":%llu}",
             sysinfo_cpu_percent(),
             (unsigned long long) sysinfo_rss_kb(),
             (unsigned long long) log_total_count(),
             (unsigned long long) log_level_count(LOG_LEVEL_ERROR),
             (unsigned long long) log_level_count(LOG_LEVEL_WARN));

    if (server->config.config_path[0] != 0) {
        json_raw(&writer, ",\"config_file\":");
        json_string(&writer, server->config.config_path);
    }

    char error[256] = "";

    latest_error(error, sizeof(error));

    if (error[0] != 0) {
        json_raw(&writer, ",\"last_error\":");
        json_string(&writer, error);
    }

    json_raw(&writer, ",\"interfaces\":[");

    for (size_t i = 0; i < interface_count; i++) {
        json_raw(&writer, "%s{\"name\":", i ? "," : "");
        json_string(&writer, interfaces[i].name);
        json_raw(&writer, ",\"ip\":");
        json_string(&writer, interfaces[i].ip);
        json_raw(&writer, "}");
    }

    json_raw(&writer, "]}");

    if (json_finish(&writer) != 0) {
        log_error("http", "/api/status payload exceeded %d bytes",
                  JSON_STATUS_CAPACITY);
        reply_error(server, connection, 500, "status payload overflow");
        return;
    }

    reply_json(server, connection, 200, buffer);
}

static void handle_stats(Server *server,
                         struct mg_connection *connection)
{
    char buffer[JSON_STATS_CAPACITY];
    JsonWriter writer;

    json_begin(&writer, buffer, sizeof(buffer));

    EncoderWorkerStats encoder_stats;

    memset(&encoder_stats, 0, sizeof(encoder_stats));

    if (server->encoder_worker != NULL) {
        encoder_worker_get_stats(server->encoder_worker, &encoder_stats);
    }

    uint64_t packets_sent = 0;
    uint64_t bytes_sent = 0;
    uint64_t retransmissions = 0;
    uint64_t send_errors = 0;
    uint64_t datagrams_rx = 0;
    uint64_t nacks = 0;
    uint64_t pli = 0;
    uint64_t rtcp_sent = 0;
    uint64_t stun_rx = 0;
    uint64_t stun_bad = 0;

    uint64_t now = now_ms();

    json_raw(&writer, "{\"uptime_sec\":%llu,\"sessions\":[",
             (unsigned long long) ((now - server->started_ms) / 1000));

    int count = 0;

    for (size_t i = 0; i < MAX_RTC_SESSIONS; i++) {
        RtcSession *session = server->sessions[i];

        if (session == NULL) {
            continue;
        }

        RtcSessionStats stats;

        memset(&stats, 0, sizeof(stats));
        rtc_session_get_stats(session, &stats);

        json_raw(&writer, "%s{\"id\":%u,\"state\":", count ? "," : "",
                 rtc_session_id(session));
        json_string(&writer, rtc_session_state_name(session));
        json_raw(&writer, ",\"peer\":");
        json_string(&writer, stats.peer);
        json_raw(&writer, ",\"signaling_peer\":");
        json_string(&writer, stats.signaling_peer);
        json_raw(&writer, ",\"h264_payload_type\":%u,\"profile_level_id\":",
                 (unsigned) stats.payload_type);
        json_string(&writer, stats.profile_level_id);
        json_raw(&writer,
                 ",\"frames_sent\":%llu,\"keyframes_sent\":%llu,"
                 "\"frames_held\":%llu,\"waiting_keyframe\":%s,"
                 "\"last_media_age_ms\":%lld,\"rr_received\":%u,"
                 "\"last_rr_age_ms\":%lld,\"browser_frames_decoded\":%llu,"
                 "\"browser_frame_size\":\"%ux%u\",\"browser_report_age_ms\":%lld",
                 (unsigned long long) stats.frames_sent,
                 (unsigned long long) stats.keyframes_sent,
                 (unsigned long long) stats.frames_held,
                 stats.waiting_keyframe ? "true" : "false",
                 stats.last_media_ms ? (long long) (now - stats.last_media_ms) : -1LL,
                 stats.rr_received,
                 stats.last_rr_ms ? (long long) (now - stats.last_rr_ms) : -1LL,
                 (unsigned long long) server->client_frames_decoded[i],
                 server->client_width[i], server->client_height[i],
                 server->client_report_ms[i]
                     ? (long long) (now - server->client_report_ms[i]) : -1LL);
        json_raw(&writer,
                 ",\"udp_port\":%u,\"rtt_ms\":%d,\"fraction_lost\":%u,"
                 "\"jitter\":%u,\"bitrate_kbps\":%.1f,\"packets_sent\":%llu,"
                 "\"bytes_sent\":%llu,\"retransmissions\":%llu,"
                 "\"send_errors\":%llu,\"datagrams_rx\":%llu,"
                 "\"rtcp_sent\":%u,\"nacks_received\":%u,\"pli_received\":%u,"
                 "\"stun_rx\":%u,\"stun_ok\":%u,\"stun_rejected\":%u,"
                 "\"peer_moved\":%u}",
                 (unsigned) (server->config.udp_base_port + i),
                 stats.rtt_ms,
                 (unsigned) stats.fraction_lost,
                 stats.jitter,
                 server->session_bitrate_kbps[i],
                 (unsigned long long) stats.packets_sent,
                 (unsigned long long) stats.bytes_sent,
                 (unsigned long long) stats.retransmissions,
                 (unsigned long long) stats.send_errors,
                 (unsigned long long) stats.datagrams_rx,
                 stats.rtcp_sent, stats.nacks_received, stats.pli_received,
                 stats.stun_rx, stats.stun_ok, stats.stun_bad,
                 stats.peer_moved);

        packets_sent += stats.packets_sent;
        bytes_sent += stats.bytes_sent;
        retransmissions += stats.retransmissions;
        send_errors += stats.send_errors;
        datagrams_rx += stats.datagrams_rx;
        nacks += stats.nacks_received;
        pli += stats.pli_received;
        rtcp_sent += stats.rtcp_sent;
        stun_rx += stats.stun_rx;
        stun_bad += stats.stun_bad;

        count++;
    }

    DtlsSrtpGlobalStats dtls_stats;

    dtls_srtp_global_stats(&dtls_stats);

    json_raw(&writer, "],\"capture\":{\"frames\":%llu,\"errors\":%llu},",
             (unsigned long long) source_worker_captured(server->source_worker),
             (unsigned long long) source_worker_errors(server->source_worker));

    json_raw(&writer,
             "\"encoder\":{\"frames\":%llu,\"keyframes\":%llu,"
             "\"params_prepended\":%llu,\"no_output\":%llu,"
             "\"skipped_idle\":%llu,\"skipped_size\":%llu,"
             "\"skipped_mismatch\":%llu,\"au_dropped\":%llu},",
             (unsigned long long) encoder_stats.frames_encoded,
             (unsigned long long) encoder_stats.keyframes,
             (unsigned long long) encoder_stats.params_prepended,
             (unsigned long long) encoder_stats.no_output,
             (unsigned long long) encoder_stats.skipped_idle,
             (unsigned long long) encoder_stats.skipped_bad_size,
             (unsigned long long) encoder_stats.skipped_mismatch,
             server->ring != NULL ?
                 (unsigned long long) au_ring_dropped(server->ring) : 0);

    json_raw(&writer,
             "\"rtp\":{\"packets_sent\":%llu,\"bytes_sent\":%llu,"
             "\"bitrate_kbps\":%.1f,\"retransmissions\":%llu,"
             "\"nacks_received\":%llu,\"pli_received\":%llu,"
             "\"rtcp_sent\":%llu,\"send_errors\":%llu},",
             (unsigned long long) packets_sent,
             (unsigned long long) bytes_sent,
             server->bitrate_kbps,
             (unsigned long long) retransmissions,
             (unsigned long long) nacks,
             (unsigned long long) pli,
             (unsigned long long) rtcp_sent,
             (unsigned long long) send_errors);

    json_raw(&writer,
             "\"ice\":{\"stun_rx\":%llu,\"stun_rejected\":%llu,"
             "\"datagrams_rx\":%llu},\"dtls\":{\"handshakes_started\":%llu,"
             "\"handshakes_completed\":%llu,\"handshake_failures\":%llu,"
             "\"fingerprint_mismatches\":%llu},",
             (unsigned long long) stun_rx,
             (unsigned long long) stun_bad,
             (unsigned long long) datagrams_rx,
             (unsigned long long) dtls_stats.handshakes_started,
             (unsigned long long) dtls_stats.handshakes_completed,
             (unsigned long long) dtls_stats.handshake_failures,
             (unsigned long long) dtls_stats.fingerprint_mismatches);

    json_raw(&writer,
             "\"events\":{\"http_requests\":%llu,\"http_errors\":%llu,"
             "\"sessions_created\":%llu,\"sessions_closed\":%llu,"
             "\"session_create_failures\":%llu,\"camera_restarts\":%llu,"
             "\"webrtc_restarts\":%llu,\"config_reloads\":%llu,"
             "\"pipeline_recoveries\":%llu,\"tls_rejected\":%llu}}",
             (unsigned long long) server->http_requests,
             (unsigned long long) server->http_errors,
             (unsigned long long) server->sessions_total,
             (unsigned long long) server->sessions_closed,
             (unsigned long long) server->session_create_failures,
             (unsigned long long) server->camera_restarts,
             (unsigned long long) server->webrtc_restarts,
             (unsigned long long) server->config_reloads,
             (unsigned long long) server->pipeline_recoveries,
             (unsigned long long) server->tls_rejected);

    if (json_finish(&writer) != 0) {
        log_error("http", "/api/stats payload exceeded %d bytes",
                  JSON_STATS_CAPACITY);
        reply_error(server, connection, 500, "stats payload overflow");
        return;
    }

    reply_json(server, connection, 200, buffer);
}

static void handle_logs(Server *server,
                        struct mg_connection *connection,
                        struct mg_http_message *message)
{
    char buffer[JSON_LOGS_CAPACITY];

    unsigned limit = 100;
    LogLevel minimum = LOG_LEVEL_DEBUG;

    /* ?limit=<1..256>&level=error|warn|info|debug */
    const char *query = NULL;
    size_t query_len = 0;

    for (size_t i = 0; i < message->uri.len; i++) {
        if (message->uri.buf[i] == '?') {
            query = message->uri.buf + i + 1;
            query_len = message->uri.len - i - 1;
            break;
        }
    }

    if (query != NULL) {
        char params[256];

        size_t copy = query_len < sizeof(params) - 1 ? query_len
                                                     : sizeof(params) - 1;
        memcpy(params, query, copy);
        params[copy] = 0;

        char *limit_param = strstr(params, "limit=");

        if (limit_param != NULL) {
            long value = strtol(limit_param + 6, NULL, 10);

            if (value > 0 && value <= LOG_RING_CAPACITY) {
                limit = (unsigned) value;
            } else {
                reply_error(server, connection, 400,
                            "limit must be between 1 and 256");
                return;
            }
        }

        char *level_param = strstr(params, "level=");

        if (level_param != NULL) {
            if (strncmp(level_param + 6, "error", 5) == 0) {
                minimum = LOG_LEVEL_ERROR;
            } else if (strncmp(level_param + 6, "warn", 4) == 0) {
                minimum = LOG_LEVEL_WARN;
            } else if (strncmp(level_param + 6, "info", 4) == 0) {
                minimum = LOG_LEVEL_INFO;
            } else if (strncmp(level_param + 6, "debug", 5) == 0) {
                minimum = LOG_LEVEL_DEBUG;
            } else {
                reply_error(server, connection, 400,
                            "level must be error, warn, info or debug");
                return;
            }
        }
    }

    size_t written = 0;

    buffer[written++] = '{';

    int n = snprintf(buffer + written, sizeof(buffer) - written,
                     "\"count\":%llu,\"errors\":%llu,\"warnings\":%llu,"
                     "\"level\":\"%s\",\"entries\":",
                     (unsigned long long) log_total_count(),
                     (unsigned long long) log_level_count(LOG_LEVEL_ERROR),
                     (unsigned long long) log_level_count(LOG_LEVEL_WARN),
                     log_level_name(minimum));

    if (n < 0 || (size_t) n >= sizeof(buffer) - written) {
        reply_error(server, connection, 500, "log payload overflow");
        return;
    }

    written += (size_t) n;

    size_t remaining = sizeof(buffer) - written - 1;
    size_t entries = log_recent_json(buffer + written, remaining, limit,
                                     minimum);

    if (entries == 0) {
        reply_error(server, connection, 500, "log payload overflow");
        return;
    }

    written += entries;

    if (written + 2 >= sizeof(buffer)) {
        reply_error(server, connection, 500, "log payload overflow");
        return;
    }

    buffer[written++] = '}';
    buffer[written] = 0;

    reply_json(server, connection, 200, buffer);
}

static void handle_camera_restart(Server *server,
                                  struct mg_connection *connection)
{
    server->camera_restarts++;

    log_info("media", "camera restart requested");

    media_pipeline_stop(server);

    /* An explicit restart retries the configured encoder preference. */
    server->encoder_override[0] = 0;

    char error[256] = "";
    char payload[512];
    JsonWriter writer;

    json_begin(&writer, payload, sizeof(payload));

    if (media_pipeline_start(server, error, sizeof(error)) != 0) {
        snprintf(server->pipeline_error, sizeof(server->pipeline_error),
                 "%s", error);
        server->pipeline_failures = 1;
        pipeline_schedule_retry(server, now_ms());

        json_raw(&writer, "{\"restarted\":false,\"error\":");
        json_string(&writer, error);
        json_raw(&writer, "}");

        if (json_finish(&writer) != 0) {
            reply_error(server, connection, 500, "restart reply overflow");
            return;
        }

        reply_json(server, connection, 503, payload);
        return;
    }

    server->pipeline_error[0] = 0;
    server->pipeline_failures = 0;
    server->pipeline_retry_ms = 0;

    /* Every viewer needs a keyframe from the rebuilt encoder. */
    atomic_store(&server->force_idr, 1);

    json_raw(&writer, "{\"restarted\":true,\"source\":");
    json_string(&writer, server->source->name);
    json_raw(&writer, ",\"encoder\":");
    json_string(&writer, server->encoder_name);
    json_raw(&writer, "}");

    if (json_finish(&writer) != 0) {
        reply_error(server, connection, 500, "restart reply overflow");
        return;
    }

    reply_json(server, connection, 200, payload);
}

static void handle_webrtc_restart(Server *server,
                                  struct mg_connection *connection)
{
    server->webrtc_restarts++;

    int closed = sessions_active(server);

    log_info("rtc", "WebRTC restart requested: dropping %d session(s)",
             closed);

    sessions_close_all(server);

    dtls_srtp_global_shutdown();

    server->dtls_ready = dtls_srtp_global_init() == 0;

    char payload[512];
    JsonWriter writer;

    json_begin(&writer, payload, sizeof(payload));
    json_raw(&writer, "{\"restarted\":%s,\"sessions_closed\":%d,"
                      "\"dtls\":\"%s\",\"fingerprint\":",
             server->dtls_ready ? "true" : "false",
             closed,
             server->dtls_ready ? "ready" : "failed");
    json_string(&writer, server->dtls_ready ? dtls_srtp_local_fingerprint()
                                            : "none");
    json_raw(&writer, "}");

    if (json_finish(&writer) != 0) {
        reply_error(server, connection, 500, "restart reply overflow");
        return;
    }

    reply_json(server, connection, server->dtls_ready ? 200 : 503, payload);
}

/*
 * Append "name" to a JSON list of names, skipping the comma for the
 * first entry. Silent when the list is full: the response then reports
 * fewer names, never invalid JSON.
 */
static void note_change(char *list, size_t list_size, size_t *length,
                        const char *name)
{
    int written = snprintf(list + *length, list_size - *length, "%s\"%s\"",
                           *length > 0 ? "," : "", name);

    if (written > 0 && (size_t) written < list_size - *length) {
        *length += (size_t) written;
    }
}

/*
 * Apply a validated configuration to the running process. Only values
 * that can change without interrupting the pipeline are applied; the
 * rest are reported as restart_required so the operator decides when to
 * restart rather than having the server silently reconfigure itself.
 */
static void apply_runtime_config(Server *server,
                                 const AppConfig *candidate,
                                 char *applied,
                                 size_t applied_size,
                                 char *restart_required,
                                 size_t restart_size)
{
    size_t applied_len = 0;
    size_t restart_len = 0;

    applied[0] = 0;
    restart_required[0] = 0;

    if (candidate->bitrate_kbps != server->config.bitrate_kbps) {
        /*
         * The encode thread applies this before its next encode call;
         * see encoder_worker_request_bitrate().
         */
        if (encoder_worker_request_bitrate(server->encoder_worker,
                                           candidate->bitrate_kbps) == 0) {
            server->config.bitrate_kbps = candidate->bitrate_kbps;
            note_change(applied, applied_size, &applied_len, "bitrate_kbps");
        } else {
            note_change(restart_required, restart_size, &restart_len,
                        "bitrate_kbps");
        }
    }

    if (candidate->verbose != server->config.verbose) {
        log_set_level(candidate->verbose ? LOG_LEVEL_DEBUG : LOG_LEVEL_INFO);
        server->config.verbose = candidate->verbose;
        note_change(applied, applied_size, &applied_len, "verbose");
    }

    struct {
        const char *name;
        int changed;
    } checks[] = {
        { "width", candidate->width != server->config.width },
        { "height", candidate->height != server->config.height },
        { "fps", candidate->fps != server->config.fps },
        { "keyframe_seconds",
          candidate->keyframe_seconds != server->config.keyframe_seconds },
        { "source", candidate->source_kind != server->config.source_kind },
        { "device", strcmp(candidate->device, server->config.device) != 0 },
        { "rpicam_bin",
          strcmp(candidate->rpicam_bin, server->config.rpicam_bin) != 0 },
        { "encoder", strcmp(candidate->encoder, server->config.encoder) != 0 },
        { "listen", strcmp(candidate->listen, server->config.listen) != 0 },
        { "http_port", candidate->http_port != server->config.http_port },
        { "udp_port",
          candidate->udp_base_port != server->config.udp_base_port },
        { "mdns", candidate->mdns != server->config.mdns },
        { "mdns_name",
          strcmp(candidate->mdns_name, server->config.mdns_name) != 0 },
        { "mdns_port", candidate->mdns_port != server->config.mdns_port }
    };

    for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
        if (checks[i].changed) {
            note_change(restart_required, restart_size, &restart_len,
                        checks[i].name);
        }
    }
}

static void handle_config_reload(Server *server,
                                 struct mg_connection *connection)
{
    if (server->config.config_path[0] == 0) {
        reply_error(server, connection, 400,
                    "no config file in use: start camstream with "
                    "--config PATH to enable reload");
        return;
    }

    server->config_reloads++;

    /*
     * Start from defaults so that a key deleted from the file falls
     * back to its default value instead of keeping the old one. Every
     * value, including ports and the source kind, is read from the file
     * during reload; command line options only apply at startup.
     */
    AppConfig candidate;

    app_config_defaults(&candidate);
    snprintf(candidate.config_path, sizeof(candidate.config_path), "%s",
             server->config.config_path);

    char error[256] = "";

    if (app_config_load_file(&candidate, server->config.config_path,
                             error, sizeof(error)) != 0) {
        reply_error(server, connection, 400, error);
        return;
    }

    if (app_config_validate(&candidate, error, sizeof(error)) != 0) {
        reply_error(server, connection, 400, error);
        return;
    }

    char applied[512] = "";
    char restart_required[512] = "";

    apply_runtime_config(server, &candidate, applied, sizeof(applied),
                         restart_required, sizeof(restart_required));

    log_info("config", "reload from %s: applied=[%s] restart_required=[%s]",
             server->config.config_path, applied, restart_required);

    char payload[1400];
    JsonWriter writer;

    json_begin(&writer, payload, sizeof(payload));
    json_raw(&writer, "{\"file\":");
    json_string(&writer, server->config.config_path);
    json_raw(&writer, ",\"applied\":[%s],\"restart_required\":[%s]}",
             applied, restart_required);

    if (json_finish(&writer) != 0) {
        reply_error(server, connection, 500, "reload reply overflow");
        return;
    }

    reply_json(server, connection, 200, payload);
}

static void handle_index(struct mg_connection *connection)
{
    mg_http_reply(connection, 200,
                  "Content-Type: text/html; charset=utf-8\r\n"
                  "Cache-Control: no-store\r\n",
                  "%s", (const char *) web_index_html);
}

/* ------------------------------------------------------------------ */
/* HTTP routing                                                        */
/* ------------------------------------------------------------------ */

static void http_event_handler(struct mg_connection *connection,
                               int event,
                               void *event_data)
{
    Server *server = connection->fn_data;

    if (server == NULL) {
        return;
    }

    if (event != MG_EV_HTTP_MSG || event_data == NULL) {
        return;
    }

    struct mg_http_message *message = event_data;
    if (message->method.buf == NULL) {
        return;
    }

    server->http_requests++;

    int is_get = mg_strcmp(message->method, mg_str("GET")) == 0;
    int is_post = mg_strcmp(message->method, mg_str("POST")) == 0;

    if (is_get && (uri_is(&message->uri, "/") ||
                   uri_is(&message->uri, "/index.html"))) {
        handle_index(connection);
        return;
    }

    if (is_get && uri_is(&message->uri, "/api/status")) {
        handle_status(server, connection);
        return;
    }

    if (is_get && uri_is(&message->uri, "/api/stats")) {
        handle_stats(server, connection);
        return;
    }

    if (is_get && uri_is(&message->uri, "/api/logs")) {
        handle_logs(server, connection, message);
        return;
    }

    if (is_post && uri_is(&message->uri, "/api/webrtc/offer")) {
        handle_offer(server, connection, message);
        return;
    }

    if (is_post && uri_is(&message->uri, "/api/webrtc/close")) {
        handle_close(server, connection, message);
        return;
    }

    if (is_post && uri_is(&message->uri, "/api/webrtc/client-stats")) {
        handle_client_stats(server, connection, message);
        return;
    }

    if (is_post && uri_is(&message->uri, "/api/camera/restart")) {
        handle_camera_restart(server, connection);
        return;
    }

    if (is_post && uri_is(&message->uri, "/api/webrtc/restart")) {
        handle_webrtc_restart(server, connection);
        return;
    }

    if (is_post && uri_is(&message->uri, "/api/config/reload")) {
        handle_config_reload(server, connection);
        return;
    }

    if (is_get || is_post) {
        reply_error(server, connection, 404, "unknown endpoint");
        return;
    }

    server->http_errors++;
    mg_http_reply(connection, 405,
                  "Content-Type: application/json\r\nAllow: GET, POST\r\n",
                  "{\"error\":\"method not allowed\"}");
}

/* ------------------------------------------------------------------ */
/* Rate sampling                                                       */
/* ------------------------------------------------------------------ */

/*
 * Counter delta that survives a pipeline rebuild: the new workers start
 * again from zero, and an unsigned "new - old" would then report an
 * absurd frame rate for one window.
 */
static uint64_t counter_delta(uint64_t current, uint64_t previous)
{
    return current >= previous ? current - previous : current;
}

static void sample_rates(Server *server, uint64_t now)
{
    uint64_t elapsed = now - server->sample_ms;

    if (elapsed < RATE_WINDOW_MS) {
        return;
    }

    uint64_t captured = source_worker_captured(server->source_worker);
    uint64_t encoded = encoder_worker_frames_encoded(server->encoder_worker);

    uint64_t bytes_sent = 0;

    for (size_t i = 0; i < MAX_RTC_SESSIONS; i++) {
        if (server->sessions[i] == NULL) {
            continue;
        }

        RtcSessionStats stats;

        memset(&stats, 0, sizeof(stats));
        rtc_session_get_stats(server->sessions[i], &stats);

        bytes_sent += stats.bytes_sent;

        if (server->session_sample_ms[i] == 0) {
            server->session_sample_ms[i] = now;
            server->session_sample_bytes[i] = stats.bytes_sent;
            continue;
        }

        uint64_t window = now - server->session_sample_ms[i];

        if (window > 0) {
            uint64_t delta = counter_delta(stats.bytes_sent,
                                           server->session_sample_bytes[i]);

            server->session_bitrate_kbps[i] =
                (double) delta * 8.0 / (double) window;
        }

        server->session_sample_ms[i] = now;
        server->session_sample_bytes[i] = stats.bytes_sent;
    }

    server->capture_fps =
        (double) counter_delta(captured, server->sample_captured) * 1000.0 /
        (double) elapsed;
    server->encode_fps =
        (double) counter_delta(encoded, server->sample_encoded) * 1000.0 /
        (double) elapsed;

    /* Sessions come and go, so the byte total can shrink: no rate then. */
    uint64_t delta_bytes = bytes_sent >= server->sample_bytes_sent
                               ? bytes_sent - server->sample_bytes_sent : 0;

    server->bitrate_kbps = (double) delta_bytes * 8.0 / (double) elapsed;

    server->sample_ms = now;
    server->sample_captured = captured;
    server->sample_encoded = encoded;
    server->sample_bytes_sent = bytes_sent;
}

/* ------------------------------------------------------------------ */
/* Main loop                                                           */
/* ------------------------------------------------------------------ */

static void drain_session_socket(RtcSession *session)
{
    uint8_t buffer[2048];
    struct sockaddr_storage source;
    int fd = rtc_session_fd(session);

    /*
     * Bounded drain: a viewer flooding its socket must not starve HTTP
     * handling or the other sessions.
     */
    for (int n = 0; n < MAX_DATAGRAMS_PER_POLL; n++) {
        socklen_t source_len = sizeof(source);

        ssize_t received = recvfrom(fd, buffer, sizeof(buffer), 0,
                                    (struct sockaddr *) &source, &source_len);

        if (received <= 0) {
            break;
        }

        rtc_session_on_udp(session, buffer, (size_t) received, &source);

        if (rtc_session_state(session) == RTC_CLOSED) {
            break;
        }
    }
}

/*
 * One poll() for everything the main thread serves: HTTP sockets (the
 * listener and every accepted connection), the session UDP sockets, the
 * mDNS socket and the AU ring's eventfd. The thread sleeps until one of
 * them is ready or a timer is due, so an idle server costs no CPU, HTTP
 * is answered immediately even while media flows, and an encoded frame
 * is sent the moment it leaves the encoder instead of on the next tick.
 *
 * Mongoose is then polled with a zero timeout: it re-checks readiness
 * itself (epoll), so this loop only decides when to wake up.
 */
static void server_poll(Server *server)
{
    struct pollfd fds[MAX_POLL_FDS];
    RtcSession *owners[MAX_POLL_FDS];
    nfds_t count = 0;
    int timeout = IDLE_WAKE_MS;
    int http_busy = 0;
    size_t http_fds = 0;

    for (struct mg_connection *c = server->mgr.conns; c != NULL; c = c->next) {
        int fd = (int) (size_t) c->fd;

        /* Closing, or draining with nothing left to send: mongoose frees
         * it on its next poll, so do not sleep before that. */
        if (c->is_closing || (c->is_draining && c->send.len == 0)) {
            http_busy = 1;
            continue;
        }

        if (fd < 0 || http_fds >= MAX_HTTP_POLL_FDS) {
            http_busy = 1;          /* cannot watch it: keep polling */
            continue;
        }

        fds[count].fd = fd;
        fds[count].events = POLLIN;
        fds[count].revents = 0;

        if (!c->is_listening && (c->send.len > 0 || c->is_connecting)) {
            fds[count].events |= POLLOUT;
        }

        owners[count] = NULL;
        count++;
        http_fds++;
    }

    for (size_t i = 0; i < MAX_RTC_SESSIONS; i++) {
        RtcSession *session = server->sessions[i];

        if (session == NULL || rtc_session_state(session) == RTC_CLOSED) {
            continue;
        }

        if (timeout > SESSION_WAKE_MS) {
            timeout = SESSION_WAKE_MS;  /* SR, keepalive and idle timers */
        }

        int dtls_timeout = rtc_session_dtls_timeout_ms(session);

        if (dtls_timeout >= 0 && dtls_timeout < timeout) {
            timeout = dtls_timeout;
        }

        int fd = rtc_session_fd(session);

        if (fd >= 0) {
            fds[count].fd = fd;
            fds[count].events = POLLIN;
            fds[count].revents = 0;
            owners[count] = session;
            count++;
        }
    }

    int mdns_socket = server->mdns != NULL ? mdns_fd(server->mdns) : -1;

    if (mdns_socket >= 0) {
        fds[count].fd = mdns_socket;
        fds[count].events = POLLIN;
        fds[count].revents = 0;
        owners[count] = NULL;
        count++;
    }

    int ring_fd = server->ring != NULL ? au_ring_fd(server->ring) : -1;

    if (ring_fd >= 0) {
        fds[count].fd = ring_fd;
        fds[count].events = POLLIN;
        fds[count].revents = 0;
        owners[count] = NULL;
        count++;
    } else if (server->ring != NULL && atomic_load(&server->media_active) &&
               timeout > 5) {
        timeout = 5;                /* no eventfd: poll the ring instead */
    }

    if (http_busy) {
        timeout = 0;
    }

    int ready = poll(fds, count, timeout);

    if (ready < 0 && errno != EINTR) {
        log_warn("net", "poll failed: %s", strerror(errno));
        return;
    }

    for (nfds_t i = 0; ready > 0 && i < count; i++) {
        if (owners[i] != NULL && (fds[i].revents & (POLLIN | POLLERR))) {
            drain_session_socket(owners[i]);
        }
    }
}

static void fan_out_access_units(Server *server)
{
    /*
     * Scratch copy of the access unit. One static buffer (not on the
     * stack: 512 KB) owned by the main loop thread, which is also the
     * only thread that pops the ring.
     */
    static uint8_t au_buffer[AU_SLOT_CAPACITY];
    AuMeta meta;

    while (au_ring_pop(server->ring, au_buffer, sizeof(au_buffer), &meta)) {
        /*
         * An access unit was lost between encoder and sender (ring
         * overrun). Every later P frame references the missing one, so
         * the decoders need a fresh IDR; the sessions keep sending what
         * they have meanwhile and the browser conceals until it arrives.
         */
        if (meta.discontinuity) {
            server->au_discontinuities++;
            atomic_store(&server->force_idr, 1);
        }

        for (size_t i = 0; i < MAX_RTC_SESSIONS; i++) {
            RtcSession *session = server->sessions[i];

            if (session != NULL &&
                rtc_session_state(session) == RTC_STREAMING) {
                rtc_session_send_access_unit(session, au_buffer, meta.size,
                                             meta.pts_us, meta.is_idr);
            }
        }
    }
}

static void reap_sessions(Server *server)
{
    int active = 0;

    for (size_t i = 0; i < MAX_RTC_SESSIONS; i++) {
        RtcSession *session = server->sessions[i];

        if (session == NULL) {
            continue;
        }

        if (rtc_session_state(session) == RTC_CLOSED) {
            rtc_session_destroy(session);
            server->sessions[i] = NULL;
        } else {
            active++;
        }
    }

    /* Idle operation skips conversion and encoding entirely. */
    atomic_store(&server->media_active, server->pipeline_running && active > 0);
}

/*
 * Create the HTTP listener. Called before anything else is started so a
 * busy port stops the process before the camera is opened (a second
 * instance would otherwise fight the first one for the sensor).
 */
static int http_start(Server *server)
{
    char error[256] = "";

    if (http_probe_bind(server->config.listen, server->config.http_port,
                        error, sizeof(error)) != 0) {
        log_error("http", "cannot listen on %s:%u: %s", server->config.listen,
                  (unsigned) server->config.http_port, error);
        return -1;
    }

    char url[128];

    snprintf(url, sizeof(url), "http://%s:%u", server->config.listen,
             server->config.http_port);

    server->listener = mg_http_listen(&server->mgr, url, http_event_handler,
                                      server);

    if (server->listener == NULL) {
        log_error("http", "cannot listen on %s (invalid listen address?)",
                  url);
        return -1;
    }

    /*
     * Put the TLS filter in front of mongoose's HTTP parser. Accepted
     * connections inherit the listener's protocol handler.
     */
    server->http_protocol = server->listener->pfn;
    server->listener->pfn = tls_guard;

    struct sockaddr_storage bound;
    socklen_t bound_len = sizeof(bound);
    int fd = (int) (size_t) server->listener->fd;

    snprintf(server->http_bound, sizeof(server->http_bound), "%s:%u",
             server->config.listen, (unsigned) server->config.http_port);

    if (getsockname(fd, (struct sockaddr *) &bound, &bound_len) == 0 &&
        bound.ss_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *) &bound;
        char ip[INET_ADDRSTRLEN];

        inet_ntop(AF_INET, &in->sin_addr, ip, sizeof(ip));
        snprintf(server->http_bound, sizeof(server->http_bound), "%s:%u", ip,
                 (unsigned) ntohs(in->sin_port));
    }

    log_info("http", "listening on %s (TCP, plain HTTP: use http://, not "
                     "https://)", server->http_bound);

    return 0;
}

static void log_access_urls(const Server *server)
{
    InterfaceInfo interfaces[MAX_INTERFACES];
    size_t interface_count = collect_interfaces(interfaces, MAX_INTERFACES);
    int wildcard = strcmp(server->config.listen, "0.0.0.0") == 0;

    if (!wildcard) {
        log_info("app", "  open http://%s:%u/", server->config.listen,
                 server->config.http_port);
        return;
    }

    log_info("app", "  open http://127.0.0.1:%u/   (this machine)",
             server->config.http_port);

    for (size_t i = 0; i < interface_count; i++) {
        log_info("app", "  open http://%s:%u/   (%s)", interfaces[i].ip,
                 server->config.http_port, interfaces[i].name);
    }

    if (interface_count == 0) {
        log_warn("app", "no non-loopback IPv4 interface is up: other "
                        "machines cannot reach this server yet");
    }
}

static void mdns_start(Server *server)
{
    MdnsConfig mdns_config;

    memset(&mdns_config, 0, sizeof(mdns_config));
    snprintf(mdns_config.host, sizeof(mdns_config.host), "%s",
             server->config.mdns_name);
    snprintf(mdns_config.model, sizeof(mdns_config.model), "camstream %s",
             APP_VERSION);
    mdns_config.port = server->config.mdns_port;
    mdns_config.http_port = server->config.http_port;

    char mdns_error[192] = "";

    server->mdns = mdns_responder_create(&mdns_config, mdns_error,
                                         sizeof(mdns_error));

    if (server->mdns != NULL) {
        log_info("app", "  open http://%s.local:%u/   (mDNS)",
                 server->config.mdns_name, server->config.http_port);
    } else {
        /*
         * Not fatal: the web UI, signaling and the media path do not
         * depend on name resolution. The operator still has the
         * addresses printed above.
         */
        log_warn("mdns", "responder unavailable: %s; use an address from "
                         "the list above", mdns_error);
    }
}

int app_server_run(AppConfig *config, volatile sig_atomic_t *stop_flag)
{
    Server *server = calloc(1, sizeof(*server));

    if (server == NULL) {
        log_error("app", "server state allocation failed");
        return 1;
    }

    server->config = *config;
    server->stop_flag = stop_flag;
    server->started_ms = now_ms();
    server->sample_ms = server->started_ms;

    atomic_init(&server->force_idr, 0);
    atomic_init(&server->media_active, 0);

    /* No hexdumps of foreign traffic, whatever mongoose's log level. */
    mg_log_set_fn(mongoose_log_discard, NULL);
    mg_mgr_init(&server->mgr);

    if (http_start(server) != 0) {
        mg_mgr_free(&server->mgr);
        free(server);
        return 1;
    }

    server->dtls_ready = dtls_srtp_global_init() == 0;

    if (server->dtls_ready) {
        log_info("webrtc", "DTLS ready (ECDSA P-256, SRTP "
                           "AES128_CM_SHA1_80), ICE-lite, media on UDP %u-%u "
                           "(one port per viewer)",
                 server->config.udp_base_port,
                 server->config.udp_base_port + MAX_RTC_SESSIONS - 1);
    } else {
        log_error("webrtc", "DTLS initialisation failed; the HTTP interface "
                            "stays up, WebRTC signaling will return 503 until "
                            "POST /api/webrtc/restart succeeds");
    }

    /*
     * The media pipeline starts regardless of WebRTC state: the camera,
     * encoder and diagnostics must stay visible even when WebRTC is
     * broken, and the encoder only runs while a viewer is connected. A
     * failure is not fatal either: HTTP stays up to report it, and the
     * supervisor retries with a backoff.
     */
    char error[256] = "";

    if (media_pipeline_start(server, error, sizeof(error)) != 0) {
        snprintf(server->pipeline_error, sizeof(server->pipeline_error), "%s",
                 error);
        server->pipeline_failures = 1;
        log_error("media", "continuing without media: %s", error);
        pipeline_schedule_retry(server, now_ms());
    }

    log_info("app", "camstream %s ready", APP_VERSION);
    log_access_urls(server);

    if (server->config.mdns) {
        mdns_start(server);
    }

    log_info("app", "firewall: allow TCP %u and UDP %u-%u from the LAN",
             server->config.http_port, server->config.udp_base_port,
             server->config.udp_base_port + MAX_RTC_SESSIONS - 1);

    while (!*stop_flag) {
        server_poll(server);

        /*
         * HTTP after the UDP sockets: a session created here already
         * sees the datagram poll on the next iteration, and the clock is
         * re-read afterwards so a fresh session is not fed a stale
         * timestamp (which used to trip the idle timeout immediately).
         */
        mg_mgr_poll(&server->mgr, 0);

        uint64_t now = now_ms();

        if (server->mdns != NULL) {
            mdns_readable(server->mdns, now);
            mdns_service(server->mdns, now);
        }

        for (size_t i = 0; i < MAX_RTC_SESSIONS; i++) {
            if (server->sessions[i] != NULL) {
                rtc_session_tick(server->sessions[i], now);
            }
        }

        if (server->pipeline_running && server->ring != NULL) {
            fan_out_access_units(server);
        }

        reap_sessions(server);
        media_supervise(server, now);
        sample_rates(server, now);
    }

    log_info("app", "shutting down");

    /* Sends the goodbye packets that drop the name from resolver caches. */
    mdns_responder_destroy(server->mdns);
    server->mdns = NULL;

    sessions_close_all(server);

    media_pipeline_stop(server);

    mg_mgr_free(&server->mgr);

    if (server->dtls_ready) {
        dtls_srtp_global_shutdown();
    }

    free(server);

    log_info("app", "stopped cleanly");

    return 0;
}
