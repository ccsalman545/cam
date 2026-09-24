#define _POSIX_C_SOURCE 200809L

/*
 * lp_server.c
 *
 * camstream-libpeer server: pipeline ownership, the media fan-out
 * thread, HTTP signaling and viewer bookkeeping. See lp_server.h.
 */
#include "lp_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <peer.h>

#include "mongoose.h"

#include "au_ring.h"
#include "encoder_worker.h"
#include "frame_hub.h"
#include "h264_encoder.h"
#include "log.h"
#include "lp_sdp.h"
#include "lp_session.h"
#include "lp_web_assets.h"
#include "mdns.h"
#include "source_worker.h"
#include "video_source.h"

#define AU_RING_SLOTS 8
#define AU_SLOT_CAPACITY (512 * 1024)
#define HTTP_POLL_MS 20
#define PIPELINE_RETRY_MS 5000
#define STATUS_CAPACITY 8192
#define SANITIZED_CAPACITY (LP_SDP_MAX_INPUT + 1024)

/* libpeer steps the RTP timestamp by 90000 / 30 per frame. */
#define LIBPEER_RTP_FPS 30

typedef struct {
    AppConfig config;
    struct mg_mgr mgr;
    MdnsResponder *mdns;

    /* Pipeline, owned by the main thread. */
    VideoSource *source;
    FrameHub *hub;
    AuRing *ring;
    H264Encoder *encoder;
    EncoderWorker *encoder_worker;
    SourceWorker *source_worker;
    pthread_t media_thread;
    int media_thread_started;
    atomic_int media_stop;
    int pipeline_running;
    uint64_t pipeline_retry_ms;
    char encoder_name[128];
    char encoder_override[8];
    char pipeline_error[256];

    atomic_int force_idr;
    atomic_int media_active;
    _Atomic uint64_t frames_fanned_out;

    /* Viewers: the main thread adds and removes, the media thread reads. */
    pthread_mutex_t sessions_lock;
    LpSession *sessions[LP_MAX_SESSIONS];
    uint32_t next_session_id;
} Server;

static uint64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000u + (uint64_t) ts.tv_nsec / 1000000u;
}

/* ------------------------------------------------------------------ */
/* Media thread: access unit ring -> every viewer                      */
/* ------------------------------------------------------------------ */

static void *media_thread(void *arg)
{
    Server *server = arg;
    uint8_t *buffer = malloc(AU_SLOT_CAPACITY);

    if (buffer == NULL) {
        log_error("media", "fan-out buffer allocation failed");
        return NULL;
    }

    int fd = au_ring_fd(server->ring);

    while (!atomic_load(&server->media_stop)) {
        if (fd >= 0) {
            struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };

            if (poll(&pfd, 1, 100) < 0 && errno != EINTR) {
                log_error("media", "poll: %s", strerror(errno));
                break;
            }
        } else {
            usleep(2000);
        }

        AuMeta meta;

        while (au_ring_pop(server->ring, buffer, AU_SLOT_CAPACITY, &meta)) {
            LpFrame *frame = NULL;

            pthread_mutex_lock(&server->sessions_lock);

            for (size_t i = 0; i < LP_MAX_SESSIONS; i++) {
                LpSession *session = server->sessions[i];

                if (session == NULL || lp_session_finished(session)) {
                    continue;
                }

                if (frame == NULL) {
                    frame = lp_frame_create(buffer, meta.size, meta.is_idr,
                                            meta.pts_us);
                    if (frame == NULL) {
                        break;
                    }
                }

                if (meta.discontinuity) {
                    lp_session_resync(session);
                }

                lp_session_push_frame(session, frame);
            }

            pthread_mutex_unlock(&server->sessions_lock);

            if (frame != NULL) {
                atomic_fetch_add(&server->frames_fanned_out, 1);
                lp_frame_unref(frame);
            }
        }
    }

    free(buffer);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Pipeline                                                            */
/* ------------------------------------------------------------------ */

static VideoSource *create_source(const AppConfig *config)
{
    switch (config->source_kind) {
    case SOURCE_TEST:
        return test_source_create(config->width, config->height, config->fps);
    case SOURCE_CSI:
        return csi_source_create(config->rpicam_bin[0] ? config->rpicam_bin
                                                       : NULL,
                                 config->width, config->height, config->fps,
                                 config->verbose);
    case SOURCE_STDIN:
        return stdin_source_create(config->width, config->height, config->fps);
    case SOURCE_V4L2:
    default:
        return v4l2_source_create(config->device, config->width,
                                  config->height, config->fps);
    }
}

static void pipeline_stop(Server *server)
{
    if (server->media_thread_started) {
        atomic_store(&server->media_stop, 1);
        pthread_join(server->media_thread, NULL);
        server->media_thread_started = 0;
    }

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
}

static int pipeline_start(Server *server)
{
    const AppConfig *config = &server->config;
    char *error = server->pipeline_error;
    size_t error_size = sizeof(server->pipeline_error);

    server->source = create_source(config);

    if (server->source == NULL) {
        snprintf(error, error_size, "capture: cannot open source '%s' (see "
                 "the log)", config->source_name);
        goto fail;
    }

    server->hub = frame_hub_create(server->source->frame_size, 6);
    server->ring = au_ring_create(AU_SLOT_CAPACITY, AU_RING_SLOTS);

    if (server->hub == NULL || server->ring == NULL) {
        snprintf(error, error_size, "out of memory for frame buffers");
        goto fail;
    }

    const char *preference = server->encoder_override[0] != 0
                                 ? server->encoder_override
                                 : config->encoder;

    server->encoder = h264_encoder_open_flags(preference,
                                              server->source->width,
                                              server->source->height,
                                              server->source->fps,
                                              config->bitrate_kbps,
                                              config->keyframe_seconds,
                                              H264_ENCODER_SINGLE_SLICE,
                                              server->encoder_name,
                                              sizeof(server->encoder_name));

    if (server->encoder == NULL) {
        snprintf(error, error_size, "encode: no usable encoder for '%s'",
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
        snprintf(error, error_size, "encode: worker thread did not start");
        goto fail;
    }

    atomic_store(&server->media_stop, 0);

    if (pthread_create(&server->media_thread, NULL, media_thread, server) != 0) {
        snprintf(error, error_size, "media: fan-out thread did not start");
        goto fail;
    }

    server->media_thread_started = 1;
    server->source_worker = source_worker_create(server->source, server->hub);

    if (server->source_worker == NULL ||
        source_worker_start(server->source_worker) != 0) {
        snprintf(error, error_size, "capture: worker thread did not start");
        goto fail;
    }

    server->pipeline_running = 1;
    server->pipeline_error[0] = 0;
    atomic_store(&server->force_idr, 1);

    log_info("media", "pipeline running: %s %ux%u @ %u fps -> %s",
             server->source->name, server->source->width,
             server->source->height, server->source->fps,
             server->encoder_name);
    return 0;

fail:
    log_error("media", "pipeline start failed: %s", error);
    pipeline_stop(server);
    return -1;
}

/*
 * Rebuild the pipeline when the camera or the encoder stops for good.
 * A hardware encoder that fails under encoder=auto is replaced by
 * libx264 at once; anything else is retried every few seconds.
 */
static void pipeline_supervise(Server *server, uint64_t now)
{
    if (server->pipeline_running) {
        int encoder_failed = encoder_worker_failed(server->encoder_worker);
        int source_failed = source_worker_failed(server->source_worker);

        if (!encoder_failed && !source_failed) {
            return;
        }

        if (encoder_failed &&
            h264_encoder_kind(server->encoder) == H264_ENCODER_HW &&
            strcmp(server->config.encoder, "auto") == 0) {
            snprintf(server->encoder_override,
                     sizeof(server->encoder_override), "sw");
            log_warn("media", "hardware encoder failed; switching to libx264");
            server->pipeline_retry_ms = now;
        } else {
            log_error("media", "%s stopped; rebuilding the pipeline",
                      encoder_failed ? "encoder" : "capture");
            server->pipeline_retry_ms = now + PIPELINE_RETRY_MS;
        }

        snprintf(server->pipeline_error, sizeof(server->pipeline_error),
                 "%s stopped at runtime", encoder_failed ? "encode" : "capture");
        pipeline_stop(server);
        return;
    }

    if (now < server->pipeline_retry_ms) {
        return;
    }

    if (pipeline_start(server) != 0) {
        server->pipeline_retry_ms = now + PIPELINE_RETRY_MS;
    }
}

/* ------------------------------------------------------------------ */
/* Sessions                                                            */
/* ------------------------------------------------------------------ */

static LpSession *find_session(Server *server, long id)
{
    for (size_t i = 0; i < LP_MAX_SESSIONS; i++) {
        if (server->sessions[i] != NULL &&
            (long) lp_session_id(server->sessions[i]) == id) {
            return server->sessions[i];
        }
    }

    return NULL;
}

/* Join and free sessions whose thread has ended. Main thread only. */
static void reap_sessions(Server *server)
{
    int live = 0;

    for (size_t i = 0; i < LP_MAX_SESSIONS; i++) {
        LpSession *session = server->sessions[i];

        if (session == NULL) {
            continue;
        }

        if (!lp_session_finished(session)) {
            live++;
            continue;
        }

        pthread_mutex_lock(&server->sessions_lock);
        server->sessions[i] = NULL;
        pthread_mutex_unlock(&server->sessions_lock);
        lp_session_destroy(session);
    }

    /* The encoder idles (no conversion, no encoding) without viewers. */
    atomic_store(&server->media_active, live > 0);
}

/* ------------------------------------------------------------------ */
/* JSON output                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    char *buffer;
    size_t capacity;
    size_t length;
} Json;

static void json_add(Json *json, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

static void json_add(Json *json, const char *format, ...)
{
    if (json->length >= json->capacity) {
        return;
    }

    va_list args;

    va_start(args, format);
    int written = vsnprintf(json->buffer + json->length,
                            json->capacity - json->length, format, args);
    va_end(args);

    if (written > 0) {
        json->length += (size_t) written;
    }
}

static void json_str(Json *json, const char *value)
{
    json_add(json, "\"");

    for (const char *p = value; *p != 0; p++) {
        unsigned char c = (unsigned char) *p;

        if (c == '"' || c == '\\') {
            json_add(json, "\\%c", c);
        } else if (c < 0x20) {
            json_add(json, "\\u%04x", c);
        } else {
            json_add(json, "%c", c);
        }
    }

    json_add(json, "\"");
}

static void reply_error(struct mg_connection *connection, int status,
                        const char *message)
{
    mg_http_reply(connection, status,
                  "Content-Type: application/json\r\nCache-Control: no-store\r\n",
                  "{%m:%m}\n", MG_ESC("error"), MG_ESC(message));
}

/* ------------------------------------------------------------------ */
/* HTTP handlers                                                       */
/* ------------------------------------------------------------------ */

static void handle_session_create(Server *server,
                                  struct mg_connection *connection)
{
    if (!server->pipeline_running) {
        char message[320];

        snprintf(message, sizeof(message), "camera pipeline is not running: %s",
                 server->pipeline_error[0] ? server->pipeline_error
                                           : "starting");
        reply_error(connection, 503, message);
        return;
    }

    size_t slot = LP_MAX_SESSIONS;

    for (size_t i = 0; i < LP_MAX_SESSIONS; i++) {
        if (server->sessions[i] == NULL) {
            slot = i;
            break;
        }
    }

    if (slot == LP_MAX_SESSIONS) {
        reply_error(connection, 503, "too many viewers (limit 4); close "
                                     "another tab first");
        return;
    }

    char error[192] = "";
    uint32_t id = ++server->next_session_id;
    LpSession *session = lp_session_create(id, &server->force_idr, error,
                                           sizeof(error));

    if (session == NULL) {
        log_error("libpeer", "session %u not created: %s", id, error);
        reply_error(connection, 500, error);
        return;
    }

    pthread_mutex_lock(&server->sessions_lock);
    server->sessions[slot] = session;
    pthread_mutex_unlock(&server->sessions_lock);
    atomic_store(&server->media_active, 1);

    log_info("libpeer", "session %u: offer sent", id);

    mg_http_reply(connection, 200,
                  "Content-Type: application/json\r\nCache-Control: no-store\r\n",
                  "{%m:%u,%m:%m}\n", MG_ESC("id"), (unsigned) id,
                  MG_ESC("sdp"), MG_ESC(lp_session_offer(session)));
}

static void handle_session_answer(Server *server,
                                  struct mg_connection *connection,
                                  struct mg_http_message *message)
{
    long id = mg_json_get_long(message->body, "$.id", -1);
    char *sdp = mg_json_get_str(message->body, "$.sdp");

    if (id < 0 || sdp == NULL) {
        free(sdp);
        reply_error(connection, 400, "expected {\"id\": N, \"sdp\": \"...\"}");
        return;
    }

    LpSession *session = find_session(server, id);

    if (session == NULL || lp_session_finished(session)) {
        free(sdp);
        reply_error(connection, 404, "no such session (it may have timed "
                                     "out); start again");
        return;
    }

    char *sanitized = malloc(SANITIZED_CAPACITY);
    char error[256] = "";
    LpSdpAnswerInfo info;

    if (sanitized == NULL) {
        free(sdp);
        reply_error(connection, 500, "out of memory");
        return;
    }

    if (lp_sdp_sanitize_answer(sdp, sanitized, SANITIZED_CAPACITY, &info,
                               error, sizeof(error)) != 0) {
        log_warn("libpeer", "session %ld: answer rejected: %s", id, error);
        reply_error(connection, 400, error);
        lp_session_close(session);
    } else if (lp_session_set_answer(session, sanitized,
                                     info.candidates_kept) != 0) {
        reply_error(connection, 409, "this session already has an answer");
    } else {
        log_info("libpeer", "session %ld: answer with %d candidate(s) (%d "
                 "mDNS, %d unusable dropped)", id, info.candidates_kept,
                 info.mdns_candidates, info.candidates_dropped);
        mg_http_reply(connection, 200,
                      "Content-Type: application/json\r\n"
                      "Cache-Control: no-store\r\n",
                      "{%m:true,%m:%d,%m:%d,%m:%d}\n", MG_ESC("ok"),
                      MG_ESC("candidates"), info.candidates_kept,
                      MG_ESC("mdns"), info.mdns_candidates,
                      MG_ESC("dropped"), info.candidates_dropped);
    }

    free(sanitized);
    free(sdp);
}

static void handle_session_close(Server *server,
                                 struct mg_connection *connection,
                                 struct mg_http_message *message)
{
    long id = mg_json_get_long(message->body, "$.id", -1);
    LpSession *session = id >= 0 ? find_session(server, id) : NULL;

    if (session != NULL) {
        lp_session_close(session);
    }

    mg_http_reply(connection, 200,
                  "Content-Type: application/json\r\nCache-Control: no-store\r\n",
                  "{%m:%s}\n", MG_ESC("closed"),
                  session != NULL ? "true" : "false");
}

static void handle_status(Server *server, struct mg_connection *connection)
{
    char *buffer = malloc(STATUS_CAPACITY);

    if (buffer == NULL) {
        reply_error(connection, 500, "out of memory");
        return;
    }

    Json json = { buffer, STATUS_CAPACITY, 0 };
    buffer[0] = 0;

    json_add(&json, "{\"server\":\"camstream-libpeer\",\"version\":");
    json_str(&json, APP_VERSION);
    json_add(&json, ",\"webrtc\":\"libpeer\",\"pipeline\":{\"running\":%s",
             server->pipeline_running ? "true" : "false");

    if (server->pipeline_running) {
        EncoderWorkerStats stats;

        encoder_worker_get_stats(server->encoder_worker, &stats);
        json_add(&json, ",\"source\":");
        json_str(&json, server->source->name);
        json_add(&json, ",\"width\":%u,\"height\":%u,\"fps\":%u,\"encoder\":",
                 server->source->width, server->source->height,
                 server->source->fps);
        json_str(&json, server->encoder_name);
        json_add(&json, ",\"hardware\":%s,\"captured\":%llu,\"encoded\":%llu,"
                 "\"keyframes\":%llu,\"ring_dropped\":%llu,"
                 "\"fanned_out\":%llu",
                 h264_encoder_kind(server->encoder) == H264_ENCODER_HW
                     ? "true" : "false",
                 (unsigned long long) source_worker_captured(
                     server->source_worker),
                 (unsigned long long) stats.frames_encoded,
                 (unsigned long long) stats.keyframes,
                 (unsigned long long) au_ring_dropped(server->ring),
                 (unsigned long long) atomic_load(&server->frames_fanned_out));
    }

    json_add(&json, ",\"error\":");
    json_str(&json, server->pipeline_error);
    json_add(&json, "},\"max_sessions\":%d,\"sessions\":[", LP_MAX_SESSIONS);

    int first = 1;

    for (size_t i = 0; i < LP_MAX_SESSIONS; i++) {
        if (server->sessions[i] == NULL) {
            continue;
        }

        LpSessionStats stats;

        lp_session_get_stats(server->sessions[i], &stats);
        json_add(&json, "%s{\"id\":%u,\"state\":", first ? "" : ",",
                 (unsigned) stats.id);
        json_str(&json, stats.state);
        json_add(&json, ",\"connected\":%s,\"age_ms\":%llu,\"connect_ms\":%llu,"
                 "\"candidates\":%d,\"frames_sent\":%llu,\"bytes_sent\":%llu,"
                 "\"frames_dropped\":%llu,\"keyframe_requests\":%llu,"
                 "\"multi_slice_frames\":%llu,\"end_reason\":",
                 stats.connected ? "true" : "false",
                 (unsigned long long) stats.age_ms,
                 (unsigned long long) stats.connect_ms, stats.candidates,
                 (unsigned long long) stats.frames_sent,
                 (unsigned long long) stats.bytes_sent,
                 (unsigned long long) stats.frames_dropped,
                 (unsigned long long) stats.keyframe_requests,
                 (unsigned long long) stats.multi_slice_frames);
        json_str(&json, stats.end_reason);
        json_add(&json, "}");
        first = 0;
    }

    json_add(&json, "]}\n");

    if (json.length >= json.capacity) {
        reply_error(connection, 500, "status too large");
    } else {
        mg_http_reply(connection, 200,
                      "Content-Type: application/json\r\n"
                      "Cache-Control: no-store\r\n", "%s", buffer);
    }

    free(buffer);
}

static void http_handler(struct mg_connection *connection, int event,
                         void *event_data)
{
    if (event != MG_EV_HTTP_MSG) {
        return;
    }

    Server *server = connection->fn_data;
    struct mg_http_message *message = event_data;
    int get = mg_strcmp(message->method, mg_str("GET")) == 0;
    int post = mg_strcmp(message->method, mg_str("POST")) == 0;

    if (get && (mg_match(message->uri, mg_str("/"), NULL) ||
                mg_match(message->uri, mg_str("/index.html"), NULL))) {
        mg_http_reply(connection, 200,
                      "Content-Type: text/html; charset=utf-8\r\n"
                      "Cache-Control: no-store\r\n",
                      "%s", (const char *) web_libpeer_html);
    } else if (get && mg_match(message->uri, mg_str("/api/status"), NULL)) {
        handle_status(server, connection);
    } else if (post && mg_match(message->uri, mg_str("/api/session"), NULL)) {
        handle_session_create(server, connection);
    } else if (post &&
               mg_match(message->uri, mg_str("/api/session/answer"), NULL)) {
        handle_session_answer(server, connection, message);
    } else if (post &&
               mg_match(message->uri, mg_str("/api/session/close"), NULL)) {
        handle_session_close(server, connection, message);
    } else if (get || post) {
        reply_error(connection, 404, "unknown endpoint");
    } else {
        reply_error(connection, 405, "method not allowed");
    }
}

/* ------------------------------------------------------------------ */
/* Startup                                                             */
/* ------------------------------------------------------------------ */

static void log_access_urls(const Server *server)
{
    struct ifaddrs *list = NULL;

    if (getifaddrs(&list) != 0) {
        return;
    }

    for (struct ifaddrs *ifa = list; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET ||
            (ifa->ifa_flags & IFF_UP) == 0 ||
            (ifa->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }

        char ip[INET_ADDRSTRLEN];
        const struct sockaddr_in *in = (const struct sockaddr_in *) ifa->ifa_addr;

        inet_ntop(AF_INET, &in->sin_addr, ip, sizeof(ip));
        log_info("http", "  open http://%s:%u/   (%s)", ip,
                 (unsigned) server->config.http_port, ifa->ifa_name);
    }

    freeifaddrs(list);
}

static void mdns_start(Server *server)
{
    MdnsConfig mdns_config;
    char error[192] = "";

    memset(&mdns_config, 0, sizeof(mdns_config));
    snprintf(mdns_config.host, sizeof(mdns_config.host), "%s",
             server->config.mdns_name);
    snprintf(mdns_config.model, sizeof(mdns_config.model),
             "camstream-libpeer %s", APP_VERSION);
    mdns_config.port = server->config.mdns_port;
    mdns_config.http_port = server->config.http_port;

    server->mdns = mdns_responder_create(&mdns_config, error, sizeof(error));

    if (server->mdns != NULL) {
        log_info("http", "  open http://%s.local:%u/   (mDNS)",
                 server->config.mdns_name, (unsigned) server->config.http_port);
    } else {
        log_warn("mdns", "responder unavailable: %s", error);
    }
}

int lp_server_run(const AppConfig *config, volatile sig_atomic_t *stop_flag)
{
    Server *server = calloc(1, sizeof(*server));

    if (server == NULL) {
        log_error("app", "out of memory");
        return 1;
    }

    server->config = *config;
    pthread_mutex_init(&server->sessions_lock, NULL);

    if (config->fps != LIBPEER_RTP_FPS) {
        log_warn("libpeer", "libpeer timestamps video at a fixed %d fps; "
                 "at %u fps the browser's playout clock drifts from the "
                 "camera. Use --fps %d for the lowest latency",
                 LIBPEER_RTP_FPS, config->fps, LIBPEER_RTP_FPS);
    }

    if (peer_init() != 0) {
        log_error("libpeer", "peer_init failed (libsrtp)");
        free(server);
        return 1;
    }

    mg_mgr_init(&server->mgr);

    char url[96];

    snprintf(url, sizeof(url), "http://%s:%u", config->listen,
             (unsigned) config->http_port);

    if (mg_http_listen(&server->mgr, url, http_handler, server) == NULL) {
        log_error("http", "cannot listen on %s (port in use? camstream "
                  "itself running?)", url);
        mg_mgr_free(&server->mgr);
        peer_deinit();
        free(server);
        return 1;
    }

    log_info("http", "listening on %s (plain HTTP)", url);
    log_access_urls(server);

    if (config->mdns) {
        mdns_start(server);
    }

    if (pipeline_start(server) != 0) {
        server->pipeline_retry_ms = now_ms() + PIPELINE_RETRY_MS;
    }

    while (!*stop_flag) {
        mg_mgr_poll(&server->mgr, HTTP_POLL_MS);

        uint64_t now = now_ms();

        if (server->mdns != NULL) {
            mdns_readable(server->mdns, now);
            mdns_service(server->mdns, now);
        }

        pipeline_supervise(server, now);
        reap_sessions(server);
    }

    log_info("app", "shutting down");

    for (size_t i = 0; i < LP_MAX_SESSIONS; i++) {
        if (server->sessions[i] != NULL) {
            lp_session_close(server->sessions[i]);
        }
    }

    pipeline_stop(server);

    for (size_t i = 0; i < LP_MAX_SESSIONS; i++) {
        lp_session_destroy(server->sessions[i]);
        server->sessions[i] = NULL;
    }

    mdns_responder_destroy(server->mdns);
    mg_mgr_free(&server->mgr);
    peer_deinit();
    pthread_mutex_destroy(&server->sessions_lock);
    free(server);
    return 0;
}
