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
#define JSON_STATUS_CAPACITY 4096
#define JSON_STATS_CAPACITY 8192
#define JSON_LOGS_CAPACITY 16384
#define MAX_INTERFACES 16

typedef struct {
    char name[IF_NAMESIZE + 1];
    char ip[INET_ADDRSTRLEN];
} InterfaceInfo;

typedef struct {
    /* Owned configuration copy: reloads mutate this, not the caller's. */
    AppConfig config;

    struct mg_mgr mgr;
    struct mg_connection *listener;

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

    server->encoder = h264_encoder_open(config->encoder,
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
                 config->encoder);
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

    log_info("media", "pipeline running: %s %ux%u -> %s",
             server->source->name, server->source->width,
             server->source->height, server->encoder_name);

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

static int json_get_long(const char *body, const char *field, long *out)
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
    long value = strtol(pos, &end, 10);

    if (end == pos) {
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

    log_info("rtc", "%08x: signaling complete (slot %zu, %s, %zu candidates)",
             rtc_session_id(session), slot, advertise_ip,
             session_config.candidate_count);
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

    long id = 0;

    if (json_get_long(body, "session_id", &id) != 0 || id <= 0) {
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

static const char *server_state(const Server *server)
{
    if (!server->pipeline_running) {
        return "camera-error";
    }

    int active = sessions_active(server);

    if (active == 0) {
        return "idle";
    }

    for (size_t i = 0; i < MAX_RTC_SESSIONS; i++) {
        if (server->sessions[i] != NULL &&
            rtc_session_state(server->sessions[i]) == RTC_STREAMING) {
            return "streaming";
        }
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

    json_raw(&writer, ",\"http\":{\"port\":%u,\"requests\":%llu,"
                      "\"errors\":%llu}",
             server->config.http_port,
             (unsigned long long) server->http_requests,
             (unsigned long long) server->http_errors);

    if (server->source != NULL) {
        const VideoSource *source = server->source;

        json_raw(&writer, ",\"source\":{\"kind\":");
        json_string(&writer, server->config.source_name);
        json_raw(&writer, ",\"name\":");
        json_string(&writer, source->name);
        json_raw(&writer, ",\"width\":%u,\"height\":%u,\"fps\":%u,"
                          "\"frames\":%llu,\"capture_errors\":%llu,"
                          "\"status\":\"%s\"}",
                 source->width, source->height, source->fps,
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
    json_raw(&writer, ",\"bitrate_kbps\":%u,\"keyframe_seconds\":%u,"
                      "\"frames\":%llu,\"fps\":%.1f,\"status\":\"%s\"}",
             server->config.bitrate_kbps,
             server->config.keyframe_seconds,
             (unsigned long long) encoder_worker_frames_encoded(server->encoder_worker),
             server->encode_fps,
             server->pipeline_running ? "ok" : "failed");

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
                      "\"skipped_mismatch\":%llu,\"encoder_no_output\":%llu}",
             server->bitrate_kbps, server->capture_fps, server->encode_fps,
             (unsigned long long) encoder_stats.frames_seen,
             (unsigned long long) encoder_stats.frames_encoded,
             server->ring != NULL ?
                 (unsigned long long) au_ring_dropped(server->ring) : 0,
             (unsigned long long) encoder_stats.skipped_idle,
             (unsigned long long) encoder_stats.skipped_bad_size,
             (unsigned long long) encoder_stats.skipped_mismatch,
             (unsigned long long) encoder_stats.no_output);

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

    json_raw(&writer, "{\"uptime_sec\":%llu,\"sessions\":[",
             (unsigned long long) ((now_ms() - server->started_ms) / 1000));

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
             "\"encoder\":{\"frames\":%llu,\"no_output\":%llu,"
             "\"skipped_idle\":%llu,\"skipped_size\":%llu,"
             "\"skipped_mismatch\":%llu,\"au_dropped\":%llu},",
             (unsigned long long) encoder_stats.frames_encoded,
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
             "\"webrtc_restarts\":%llu,\"config_reloads\":%llu}}",
             (unsigned long long) server->http_requests,
             (unsigned long long) server->http_errors,
             (unsigned long long) server->sessions_total,
             (unsigned long long) server->sessions_closed,
             (unsigned long long) server->session_create_failures,
             (unsigned long long) server->camera_restarts,
             (unsigned long long) server->webrtc_restarts,
             (unsigned long long) server->config_reloads);

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

    char error[256] = "";
    char payload[512];
    JsonWriter writer;

    json_begin(&writer, payload, sizeof(payload));

    if (media_pipeline_start(server, error, sizeof(error)) != 0) {
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
    json_string(&writer, dtls_srtp_local_fingerprint());
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
                  "Cache-Control: no-store\r\n"
                  "Connection: close\r\n",
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
            uint64_t delta = stats.bytes_sent - server->session_sample_bytes[i];

            server->session_bitrate_kbps[i] =
                (double) delta * 8.0 / (double) window;
        }

        server->session_sample_ms[i] = now;
        server->session_sample_bytes[i] = stats.bytes_sent;
    }

    server->capture_fps =
        (double) (captured - server->sample_captured) * 1000.0 / (double) elapsed;
    server->encode_fps =
        (double) (encoded - server->sample_encoded) * 1000.0 / (double) elapsed;

    if (server->sample_ms != 0) {
        uint64_t delta_bytes = bytes_sent >= server->sample_bytes_sent ?
            bytes_sent - server->sample_bytes_sent : 0;

        server->bitrate_kbps =
            (double) delta_bytes * 8.0 / (double) elapsed;
    }

    server->sample_ms = now;
    server->sample_captured = captured;
    server->sample_encoded = encoded;
    server->sample_bytes_sent = bytes_sent;
}

/* ------------------------------------------------------------------ */
/* Main loop                                                           */
/* ------------------------------------------------------------------ */

static void poll_sessions(Server *server)
{
    struct pollfd fds[MAX_RTC_SESSIONS];
    int fd_count = 0;
    int timeout = 10;
    int has_sessions = 0;

    for (size_t i = 0; i < MAX_RTC_SESSIONS; i++) {
        RtcSession *session = server->sessions[i];

        if (session == NULL ||
            rtc_session_state(session) == RTC_CLOSED) {
            continue;
        }

        has_sessions = 1;

        int fd = rtc_session_fd(session);

        if (fd >= 0 && fd_count < MAX_RTC_SESSIONS) {
            fds[fd_count].fd = fd;
            fds[fd_count].events = POLLIN;
            fds[fd_count].revents = 0;
            fd_count++;
        }

        int dtls_timeout = rtc_session_dtls_timeout_ms(session);

        if (dtls_timeout >= 0 && dtls_timeout < timeout) {
            timeout = dtls_timeout > 0 ? dtls_timeout : 0;
        }
    }

    if (!has_sessions) {
        /* Idle: slower wakeups, but still responsive to HTTP. */
        timeout = 100;
    }

    if (fd_count > 0) {
        int ready = poll(fds, (nfds_t) fd_count, timeout);

        if (ready < 0 && errno != EINTR) {
            log_warn("net", "poll failed: errno=%d (%s)", errno,
                     strerror(errno));
            return;
        }
    } else {
        poll(NULL, 0, timeout);
    }

    for (int i = 0; i < fd_count; i++) {
        if (!(fds[i].revents & POLLIN)) {
            continue;
        }

        for (size_t s = 0; s < MAX_RTC_SESSIONS; s++) {
            RtcSession *session = server->sessions[s];

            if (session == NULL || rtc_session_fd(session) != fds[i].fd) {
                continue;
            }

            uint8_t buffer[2048];
            struct sockaddr_storage source;

            /*
             * Bounded drain: a viewer flooding its socket must not
             * starve HTTP handling or the other sessions.
             */
            for (int n = 0; n < MAX_DATAGRAMS_PER_POLL; n++) {
                socklen_t source_len = sizeof(source);

                ssize_t received = recvfrom(fds[i].fd, buffer, sizeof(buffer),
                                            0, (struct sockaddr *) &source,
                                            &source_len);

                if (received <= 0) {
                    break;
                }

                rtc_session_on_udp(session, buffer, (size_t) received, &source);
            }
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

    server->dtls_ready = dtls_srtp_global_init() == 0;

    if (!server->dtls_ready) {
        log_error("webrtc", "DTLS initialisation failed; the HTTP interface "
                            "stays up, WebRTC signaling will return 503 until "
                            "POST /api/webrtc/restart succeeds");
    }

    /*
     * The media pipeline starts regardless of WebRTC state: the camera,
     * encoder and diagnostics must stay visible even when WebRTC is
     * broken, and the encoder only runs while a viewer is connected.
     */
    char error[256] = "";

    if (media_pipeline_start(server, error, sizeof(error)) != 0) {
        log_error("media", "continuing without media: %s", error);
    }

    mg_mgr_init(&server->mgr);

    char url[128];

    snprintf(url, sizeof(url), "http://%s:%u", server->config.listen,
             server->config.http_port);

    server->listener = mg_http_listen(&server->mgr, url, http_event_handler,
                                      server);

    if (server->listener == NULL) {
        log_error("http", "cannot listen on %s", url);
        media_pipeline_stop(server);
        mg_mgr_free(&server->mgr);
        dtls_srtp_global_shutdown();
        free(server);
        return 1;
    }

    InterfaceInfo interfaces[MAX_INTERFACES];
    size_t interface_count = collect_interfaces(interfaces, MAX_INTERFACES);

    log_info("app", "camstream %s ready", APP_VERSION);
    log_info("app", "web UI and signaling on http://%s:%u/",
             server->config.listen, server->config.http_port);

    if (interface_count == 0) {
        log_info("app", "no non-loopback interface found; use "
                        "http://localhost:%u/", server->config.http_port);
    }

    for (size_t i = 0; i < interface_count; i++) {
        log_info("app", "  open http://%s:%u/   (%s)", interfaces[i].ip,
                 server->config.http_port, interfaces[i].name);
    }

    log_info("app", "media: UDP %u-%u, one port per viewer",
             server->config.udp_base_port,
             server->config.udp_base_port + MAX_RTC_SESSIONS - 1);
    log_info("app", "firewall: allow TCP %u and UDP %u-%u",
             server->config.http_port, server->config.udp_base_port,
             server->config.udp_base_port + MAX_RTC_SESSIONS - 1);

    if (server->config.mdns) {
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
            log_info("app", "mDNS: http://%s.local:%u/ (no address needed on "
                            "the same LAN)",
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

    while (!*stop_flag) {
        poll_sessions(server);

        /*
         * HTTP last: a session created here already sees the datagram
         * poll below on the next iteration, and the clock is re-read
         * afterwards so a fresh session is not fed a stale timestamp
         * (which used to trip the idle timeout immediately).
         */
        mg_mgr_poll(&server->mgr, 0);

        uint64_t now = now_ms();

        /*
         * mDNS is served from this loop as well: the responder needs a
         * wakeup for probes, announcements and pending replies, and the
         * loop already runs at least every 100 ms while idle.
         */
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

    return 0;
}
