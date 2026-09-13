/*
 * app_server.c
 *
 * Composition root:
 *
 *   main thread      Mongoose HTTP loop (web UI + WebRTC signaling),
 *                    ICE sockets, SRTP send fan out
 *   source thread    V4L2 or test pattern capture into the hub
 *   encode thread    I420 conversion + H.264 into the AU ring
 */
#include "app_server.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifdef USE_LIBPEER
/* libpeer uses mbedTLS internally, no OpenSSL needed */
#else
#if __has_include(<openssl/rand.h>)
#include <openssl/rand.h>
#endif
#endif

#include "mongoose.h"

#include "au_ring.h"
#include "encoder_worker.h"
#include "source_worker.h"
#include "webrtc_session.h"

#define MAX_RTC_SESSIONS 8
#define AU_RING_SLOTS 8
#define AU_SLOT_CAPACITY (512 * 1024)

extern const char *web_ui_html;

/* ------------------------------------------------------------------ */
/* Server state                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    const AppConfig *config;

    struct mg_mgr mgr;
    struct mg_connection *listener;

    VideoSource *source;
    SourceWorker *source_worker;
    FrameHub *hub;

    H264Encoder *encoder;
    char encoder_name[96];
    EncoderWorker *encoder_worker;
    AuRing *ring;
    atomic_int force_idr;
    atomic_int rtc_active;

    RtcSession *sessions[MAX_RTC_SESSIONS];

    uint64_t started_ms;
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

typedef struct {
    char name[IF_NAMESIZE + 1];
    char ip[INET_ADDRSTRLEN];
} InterfaceInfo;

/*
 * Collect local IPv4 addresses, loopback excluded.
 * Only interfaces that are UP and not loopback are considered.
 */
static size_t collect_interfaces(InterfaceInfo *out, size_t max)
{
    struct ifaddrs *addrs = NULL;

    if (getifaddrs(&addrs) != 0) {
        return 0;
    }

    size_t count = 0;

    for (struct ifaddrs *ifa = addrs; ifa != NULL && count < max; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }

        if ((ifa->ifa_flags & IFF_UP) == 0) {
            continue;
        }

        if (ifa->ifa_flags & IFF_LOOPBACK) {
            continue;
        }

        const struct sockaddr_in *a = (const struct sockaddr_in *) ifa->ifa_addr;

        uint32_t host = ntohl(a->sin_addr.s_addr);

        if ((host >> 24) == 127) {
            continue;
        }

        snprintf(out[count].name, sizeof(out[count].name), "%s", ifa->ifa_name);
        inet_ntop(AF_INET, &a->sin_addr, out[count].ip, sizeof(out[count].ip));
        count++;
    }

    freeifaddrs(addrs);
    return count;
}

/*
 * Best address to advertise in SDP candidates: prefer the IP
 * the browser used to reach us, otherwise the first private
 * IPv4 address.
 * Uses inet_pton for validation instead of deprecated inet_addr.
 */
static void choose_advertise_ip(const char *host_header,
                                char *out,
                                size_t out_size)
{
    InterfaceInfo interfaces[16];
    size_t count = collect_interfaces(interfaces, 16);

    if (host_header != NULL && host_header[0] != 0) {
        char candidate[INET_ADDRSTRLEN] = "";
        size_t i = 0;

        for (; host_header[i] != 0 && host_header[i] != ':' && i + 1 < sizeof(candidate); i++) {
            candidate[i] = host_header[i];
        }
        candidate[i] = 0;

        struct in_addr addr;
        if (inet_pton(AF_INET, candidate, &addr) == 1) {
            for (size_t n = 0; n < count; n++) {
                if (strcmp(interfaces[n].ip, candidate) == 0) {
                    snprintf(out, out_size, "%s", candidate);
                    return;
                }
            }
        }
    }

    for (size_t n = 0; n < count; n++) {
        struct in_addr addr;
        if (inet_pton(AF_INET, interfaces[n].ip, &addr) != 1) {
            continue;
        }
        uint32_t host = ntohl(addr.s_addr);

        int private_range =
            ((host >> 24) == 10) ||
            ((host >> 20) == 0xAC1) ||                   /* 172.16 to 172.31 */
            ((host >> 16) == 0xC0A8) ||                  /* 192.168 */
            ((host >> 16) == 0xA9FE);                    /* 169.254 */

        if (private_range) {
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
/* JSON helpers                                                        */
/* ------------------------------------------------------------------ */

/*
 * Extract a top level JSON string field. Handles the common
 * escape sequences that appear in SDP payloads.
 */
static int json_get_string(const char *body, const char *field,
                           char *out, size_t out_size)
{
    char pattern[64];

    snprintf(pattern, sizeof(pattern), "\"%s\"", field);

    const char *pos = strstr(body, pattern);

    if (pos == NULL) {
        return -1;
    }

    pos += strlen(pattern);

    while (*pos == ' ' || *pos == ':') {
        pos++;
    }

    if (*pos != '"') {
        return -1;
    }

    pos++;

    size_t out_pos = 0;

    while (*pos != 0 && *pos != '"' && out_pos + 1 < out_size) {
        if (*pos == '\\') {
            pos++;

            switch (*pos) {
            case 'n': out[out_pos++] = '\n'; break;
            case 'r': out[out_pos++] = '\r'; break;
            case 't': out[out_pos++] = '\t'; break;
            case 'b': out[out_pos++] = '\b'; break;
            case 'f': out[out_pos++] = '\f'; break;
            case '"': out[out_pos++] = '"'; break;
            case '\\': out[out_pos++] = '\\'; break;
            case '/': out[out_pos++] = '/'; break;
            case 'u': {
                char hex[5] = { 0 };

                for (int i = 0; i < 4 && pos[1] != 0; i++) {
                    hex[i] = *++pos;
                }

                long code = strtol(hex, NULL, 16);

                if (code < 128) {
                    out[out_pos++] = (char) code;
                } else {
                    out[out_pos++] = '?';
                }
                break;
            }
            default:
                out[out_pos++] = *pos;
                break;
            }

            pos++;
        } else {
            out[out_pos++] = *pos++;
        }
    }

    out[out_pos] = 0;

    return *pos == '"' ? 0 : -1;
}

static int json_get_int(const char *body, const char *field, long *out)
{
    char pattern[64];

    snprintf(pattern, sizeof(pattern), "\"%s\"", field);

    const char *pos = strstr(body, pattern);

    if (pos == NULL) {
        return -1;
    }

    pos += strlen(pattern);

    while (*pos == ' ' || *pos == ':') {
        pos++;
    }

    char *end = NULL;
    long val = strtol(pos, &end, 10);

    if (end == pos) {
        return -1;
    }

    *out = val;

    return 0;
}

/*
 * Append a JSON escaped copy of the input string.
 */
static size_t json_escape_append(const char *input, char *out, size_t out_size)
{
    size_t written = 0;

    for (const char *p = input; *p != 0; p++) {
        char esc[8];
        size_t esc_len = 0;

        switch (*p) {
        case '"':  memcpy(esc, "\\\"", 2); esc_len = 2; break;
        case '\\': memcpy(esc, "\\\\", 2); esc_len = 2; break;
        case '\r': memcpy(esc, "\\r", 2); esc_len = 2; break;
        case '\n': memcpy(esc, "\\n", 2); esc_len = 2; break;
        case '\t': memcpy(esc, "\\t", 2); esc_len = 2; break;
        default:
            esc[0] = *p;
            esc_len = 1;
            break;
        }

        if (written + esc_len + 1 > out_size) {
            break;
        }

        memcpy(out + written, esc, esc_len);
        written += esc_len;
    }

    return written;
}

/* ------------------------------------------------------------------ */
/* Session management                                                  */
/* ------------------------------------------------------------------ */

static void session_on_idr_request(void *user)
{
    Server *server = user;

    atomic_store(&server->force_idr, 1);
}

static void session_on_closed(void *user, RtcSession *session)
{
    (void) user;

    printf("rtc %08x: closed\n", rtc_session_id(session));
}

static void handle_rtc_offer(Server *server,
                             struct mg_connection *connection,
                             struct mg_http_message *message)
{
    char request_body[16384];

    if (message->body.len >= sizeof(request_body)) {
        mg_http_reply(connection, 413, "Content-Type: application/json\r\n",
                      "{\"error\":\"signaling request is too large\"}");
        return;
    }

    memcpy(request_body, message->body.buf, message->body.len);
    request_body[message->body.len] = 0;

    char sdp[8192];

    if (json_get_string(request_body, "sdp", sdp, sizeof(sdp)) != 0) {
        mg_http_reply(connection, 400, "Content-Type: application/json\r\n",
                      "{\"error\":\"missing sdp field\"}");
        return;
    }

    SdpOffer offer;

    if (sdp_parse_offer(sdp, strlen(sdp), &offer) != 0) {
        mg_http_reply(connection, 400, "Content-Type: application/json\r\n",
                      "{\"error\":\"offer is missing ICE credentials, "
                      "fingerprint or H264 codec\"}");
        return;
    }

    /*
     * Find a free slot.
     */
    size_t slot = MAX_RTC_SESSIONS;

    for (size_t i = 0; i < MAX_RTC_SESSIONS; i++) {
        if (server->sessions[i] == NULL) {
            slot = i;
            break;
        }
    }

    if (slot == MAX_RTC_SESSIONS) {
        mg_http_reply(connection, 503, "Content-Type: application/json\r\n",
                      "{\"error\":\"session limit reached\"}");
        return;
    }

    struct mg_str *host_header_array = NULL;
    struct mg_str host = mg_str("");

    if ((host_header_array = (struct mg_str *) mg_http_get_header(message, "Host")) != NULL) {
        host = *host_header_array;
    }

    char host_copy[256] = "";
    size_t copy_len = host.len < sizeof(host_copy) - 1 ? host.len : sizeof(host_copy) - 1;
    memcpy(host_copy, host.buf, copy_len);

    char advertise_ip[INET_ADDRSTRLEN];
    choose_advertise_ip(host_copy, advertise_ip, sizeof(advertise_ip));

    /* Collect all local IPs to advertise as additional host candidates */
    InterfaceInfo all_ifaces[16];
    size_t all_count = collect_interfaces(all_ifaces, 16);

    /* Build deduplicated extra IP list (excluding primary) */
    const char *extra_ip_ptrs[16];
    char extra_ip_storage[16][INET_ADDRSTRLEN];
    size_t extra_count = 0;

    for (size_t i = 0; i < all_count && extra_count < 16; i++) {
        if (strcmp(all_ifaces[i].ip, advertise_ip) == 0) {
            continue;
        }
        /* deduplicate */
        int dup = 0;
        for (size_t j = 0; j < extra_count; j++) {
            if (strcmp(extra_ip_storage[j], all_ifaces[i].ip) == 0) {
                dup = 1;
                break;
            }
        }
        if (dup) {
            continue;
        }
        snprintf(extra_ip_storage[extra_count], sizeof(extra_ip_storage[0]),
                 "%s", all_ifaces[i].ip);
        extra_ip_ptrs[extra_count] = extra_ip_storage[extra_count];
        extra_count++;
    }

    /* Proxy detection: if Host header is not a local IP and not private, warn */
    int host_matches_local = 0;
    char host_ip_only[INET_ADDRSTRLEN] = "";
    if (host_copy[0] != 0) {
        size_t k = 0;
        for (; host_copy[k] != 0 && host_copy[k] != ':' && k + 1 < sizeof(host_ip_only); k++) {
            host_ip_only[k] = host_copy[k];
        }
        host_ip_only[k] = 0;
        for (size_t n = 0; n < all_count; n++) {
            if (strcmp(all_ifaces[n].ip, host_ip_only) == 0) {
                host_matches_local = 1;
                break;
            }
        }
        if (!host_matches_local && host_ip_only[0] != 0) {
            /* Check if host_ip_only looks like an IP (contains dot) vs hostname */
            int is_ip_like = 0;
            for (size_t c = 0; host_ip_only[c] != 0; c++) {
                if (host_ip_only[c] == '.' && host_ip_only[c+1] >= '0' && host_ip_only[c+1] <= '9') {
                    is_ip_like = 1;
                    break;
                }
                if (host_ip_only[c] >= '0' && host_ip_only[c] <= '9') {
                    is_ip_like = 1;
                }
            }
            if (!is_ip_like || strcmp(host_ip_only, advertise_ip) != 0) {
                printf("rtc: Host header '%s' does not match local interfaces (primary %s). "
                       "If this page is opened through a proxy, UDP media will fail. "
                       "Open directly via http://%s:%u/\n",
                       host_copy, advertise_ip, advertise_ip, server->config->http_port);
            }
        }
    }

    uint32_t session_id = 0;

    /* Secure random session id without requiring OpenSSL */
    {
        FILE *f = fopen("/dev/urandom", "rb");
        if (f) {
            fread(&session_id, 1, sizeof(session_id), f);
            fclose(f);
        }
#ifdef USE_LIBPEER
        /* libpeer mode - no RAND_bytes */
        if (session_id == 0) {
            session_id = (uint32_t)time(NULL) ^ (uint32_t)(uintptr_t)server ^ (uint32_t)slot;
            session_id = session_id * 2654435761u;
        }
#else
#if __has_include(<openssl/rand.h>)
        if (session_id == 0) {
            RAND_bytes((unsigned char*)&session_id, sizeof(session_id));
        }
#endif
        if (session_id == 0) {
            session_id = (uint32_t)time(NULL) ^ (uint32_t)(uintptr_t)server ^ (uint32_t)slot;
            session_id = session_id * 2654435761u;
        }
#endif
    }
    if (session_id == 0) {
        session_id = 1;
    }

    RtcSessionConfig session_config = {
        .id = session_id,
        .udp_port = (uint16_t) (server->config->udp_base_port + slot),
        .advertise_ip = advertise_ip,
        .extra_ips = extra_count ? extra_ip_ptrs : NULL,
        .extra_ip_count = extra_count,
        .offer = offer,
        .remote_sdp = sdp,
        .server = server,
        .on_idr_request = session_on_idr_request,
        .on_closed = session_on_closed
    };

    RtcSession *session = NULL;
    char answer[8192];
    size_t answer_length = 0;

    if (rtc_session_create(&session_config, &session,
                           answer, sizeof(answer), &answer_length) != 0) {
        mg_http_reply(connection, 500, "Content-Type: application/json\r\n",
                      "{\"error\":\"session create failed (UDP port busy?)\"}");
        return;
    }

    server->sessions[slot] = session;
    atomic_store(&server->rtc_active, 1);

    /*
     * A fresh viewer always needs a keyframe first.
     */
    atomic_store(&server->force_idr, 1);

    /*
     * Build the JSON response with the escaped SDP answer.
     * Answer is up to 8192, escaped it can double (CRLF -> \r\n), so payload
     * needs to be much larger than 9216. Use 24k to be safe.
     */
    char payload[24576];
    size_t offset = 0;

    int n = snprintf(payload + offset, sizeof(payload) - offset,
                     "{\"type\":\"answer\",\"session_id\":%u,"
                     "\"udp_port\":%u,\"sdp\":\"",
                     session_id,
                     (unsigned) session_config.udp_port);
    if (n < 0 || (size_t)n >= sizeof(payload) - offset) {
        mg_http_reply(connection, 500, "Content-Type: application/json\r\n",
                      "{\"error\":\"answer too large\"}");
        rtc_session_destroy(session);
        server->sessions[slot] = NULL;
        atomic_store(&server->rtc_active, 0);
        return;
    }
    offset += (size_t) n;

    size_t esc = json_escape_append(answer, payload + offset,
                                    sizeof(payload) - offset);
    if (esc == 0 && answer[0] != 0) {
        mg_http_reply(connection, 500, "Content-Type: application/json\r\n",
                      "{\"error\":\"SDP escape overflow\"}");
        rtc_session_destroy(session);
        server->sessions[slot] = NULL;
        atomic_store(&server->rtc_active, 0);
        return;
    }
    offset += esc;

    n = snprintf(payload + offset, sizeof(payload) - offset, "\"}");
    if (n < 0 || (size_t)n >= sizeof(payload) - offset) {
        mg_http_reply(connection, 500, "Content-Type: application/json\r\n",
                      "{\"error\":\"payload overflow\"}");
        rtc_session_destroy(session);
        server->sessions[slot] = NULL;
        atomic_store(&server->rtc_active, 0);
        return;
    }
    offset += (size_t) n;

    mg_http_reply(connection, 200, "Content-Type: application/json\r\n",
                  "%s", payload);

    printf("rtc %08x: signaling complete (slot %zu, %s)\n",
           session_id, slot, advertise_ip);
}

static void handle_rtc_close(Server *server,
                             struct mg_connection *connection,
                             struct mg_http_message *message)
{
    if (message->body.len >= 512) {
        mg_http_reply(connection, 413, "Content-Type: application/json\r\n",
                      "{\"error\":\"close request is too large\"}");
        return;
    }

    char request_body[512];
    memcpy(request_body, message->body.buf, message->body.len);
    request_body[message->body.len] = 0;

    long id = 0;

    if (json_get_int(request_body, "session_id", &id) != 0) {
        mg_http_reply(connection, 400, "Content-Type: application/json\r\n",
                      "{\"error\":\"missing session_id\"}");
        return;
    }

    for (size_t i = 0; i < MAX_RTC_SESSIONS; i++) {
        if (server->sessions[i] != NULL &&
            rtc_session_id(server->sessions[i]) == (uint32_t) id) {
            rtc_session_close(server->sessions[i]);
            break;
        }
    }

    mg_http_reply(connection, 200, "Content-Type: application/json\r\n",
                  "{\"closed\":true}");
}

/* ------------------------------------------------------------------ */
/* /status                                                             */
/* ------------------------------------------------------------------ */

static void handle_status(Server *server,
                          struct mg_connection *connection)
{
    InterfaceInfo interfaces[16];
    size_t interface_count = collect_interfaces(interfaces, 16);

    char payload[8192];
    size_t offset = 0;

    uint64_t uptime_s = (now_ms() - server->started_ms) / 1000;

    offset += (size_t) snprintf(payload + offset, sizeof(payload) - offset,
        "{\"version\":\"%s\","
        "\"uptime_sec\":%llu,"
        "\"source\":{\"name\":\"%s\",\"kind\":\"%s\",\"width\":%u,"
        "\"height\":%u,\"fps\":%u},"
        "\"encoder\":{\"name\":\"%s\",\"preference\":\"%s\","
        "\"bitrate_kbps\":%u,\"keyframe_seconds\":%u},"
        "\"http_port\":%u,"
        "\"udp_port\":%u,"
#ifdef USE_LIBPEER
        "\"transport\":\"webrtc-libpeer\","
#else
        "\"transport\":\"webrtc\","
#endif
        "\"captured_frames\":%llu,"
        "\"encoded_frames\":%llu,"
        "\"au_dropped\":%llu,"
        "\"sessions\":[",
        APP_VERSION,
        (unsigned long long) uptime_s,
        server->source->name,
        server->config->use_test_source ? "test" : "v4l2",
        server->source->width,
        server->source->height,
        server->source->fps,
        server->encoder_name[0] ? server->encoder_name : "none",
        server->config->encoder,
        server->config->bitrate_kbps,
        server->config->keyframe_seconds,
        server->config->http_port,
        server->config->udp_base_port,
        (unsigned long long) source_worker_captured(server->source_worker),
        (unsigned long long) encoder_worker_frames_encoded(server->encoder_worker),
        (unsigned long long) au_ring_dropped(server->ring));

    int session_count = 0;

    for (size_t i = 0; i < MAX_RTC_SESSIONS; i++) {
        RtcSession *session = server->sessions[i];

        if (session == NULL) {
            continue;
        }

        RtcSessionStats stats;
        rtc_session_get_stats(session, &stats);

        offset += (size_t) snprintf(payload + offset, sizeof(payload) - offset,
            "%s{\"id\":%u,\"state\":\"%s\",\"udp_port\":%u,"
            "\"packets_sent\":%llu,\"bytes_sent\":%llu,"
            "\"pli\":%u,\"nacks\":%u,\"retx\":%u}",
            session_count ? "," : "",
            rtc_session_id(session),
            rtc_session_state_name(session),
            server->config->udp_base_port + (unsigned) i,
            (unsigned long long) stats.packets_sent,
            (unsigned long long) stats.bytes_sent,
            stats.pli_received,
            stats.nacks_received,
            stats.retransmissions);

        session_count++;
    }

    offset += (size_t) snprintf(payload + offset, sizeof(payload) - offset,
                                "],\"interfaces\":[");

    for (size_t i = 0; i < interface_count; i++) {
        offset += (size_t) snprintf(payload + offset,
                                    sizeof(payload) - offset,
                                    "%s{\"name\":\"%s\",\"ip\":\"%s\"}",
                                    i ? "," : "",
                                    interfaces[i].name,
                                    interfaces[i].ip);
    }

    offset += (size_t) snprintf(payload + offset, sizeof(payload) - offset,
                                "]}\n");

    mg_http_reply(connection, 200, "Content-Type: application/json\r\n",
                  "%s", payload);
}

/* ------------------------------------------------------------------ */
/* HTTP events                                                         */
/* ------------------------------------------------------------------ */

static void http_event_handler(struct mg_connection *connection,
                               int event,
                               void *event_data)
{
    Server *server = connection->fn_data;

    if (server == NULL) {
        return;
    }

    switch (event) {

    case MG_EV_HTTP_MSG: {
        struct mg_http_message *message =
            (struct mg_http_message *) event_data;

        if (mg_match(message->uri, mg_str("/rtc/offer"), NULL)) {
            handle_rtc_offer(server, connection, message);
            return;
        }

        if (mg_match(message->uri, mg_str("/rtc/close"), NULL)) {
            handle_rtc_close(server, connection, message);
            return;
        }

        if (mg_match(message->uri, mg_str("/status"), NULL)) {
            handle_status(server, connection);
            return;
        }

        if (mg_match(message->uri, mg_str("/"), NULL)) {
            mg_http_reply(connection, 200,
                          "Content-Type: text/html; charset=utf-8\r\n"
                          "Cache-Control: no-store\r\n",
                          "%s", web_ui_html);
            return;
        }

        mg_http_reply(connection, 404, "Content-Type: text/plain\r\n",
                      "404 Not Found\n");
        return;
    }

    default:
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Main loop                                                           */
/* ------------------------------------------------------------------ */

int app_server_run(const AppConfig *config, volatile sig_atomic_t *stop_flag)
{
    Server *server = calloc(1, sizeof(*server));

    if (server == NULL) {
        return 1;
    }

    server->config = config;
    server->stop_flag = stop_flag;
    server->started_ms = now_ms();

    atomic_init(&server->force_idr, 0);
    atomic_init(&server->rtc_active, 0);

    if (dtls_srtp_global_init() != 0) {
        fprintf(stderr, "server: DTLS global init failed\n");
        free(server);
        return 1;
    }

    /*
     * Source.
     */
    if (config->use_test_source) {
        server->source = test_source_create(config->width,
                                            config->height,
                                            config->fps);
    } else {
        server->source = v4l2_source_create(config->device,
                                            config->width,
                                            config->height,
                                            config->fps);
    }

    if (server->source == NULL) {
        fprintf(stderr,
            "server: cannot open video source. Without a camera run\n"
            "        with --test to use the synthetic test pattern.\n");
        free(server);
        return 1;
    }

    server->hub = frame_hub_create(server->source->frame_size, 6);

    if (server->hub == NULL) {
        video_source_close(server->source);
        free(server);
        return 1;
    }

    server->source_worker = source_worker_create(server->source, server->hub);

    if (server->source_worker == NULL ||
        source_worker_start(server->source_worker) != 0) {
        fprintf(stderr, "server: source worker failed to start\n");
        goto fail;
    }

    /*
     * Encoder.
     */
    server->encoder = h264_encoder_open(config->encoder,
                                        server->source->width,
                                        server->source->height,
                                        server->source->fps,
                                        config->bitrate_kbps,
                                        config->keyframe_seconds,
                                        server->encoder_name,
                                        sizeof(server->encoder_name));

    if (server->encoder == NULL) {
        fprintf(stderr, "server: no usable encoder, see errors above\n");
        goto fail;
    }

    server->ring = au_ring_create(AU_SLOT_CAPACITY, AU_RING_SLOTS);

    server->encoder_worker = encoder_worker_create(server->hub,
                                                   server->encoder,
                                                   server->source->width,
                                                   server->source->height,
                                                   server->ring,
                                                   &server->force_idr,
                                                   &server->rtc_active);

    if (server->ring == NULL || server->encoder_worker == NULL ||
        encoder_worker_start(server->encoder_worker) != 0) {
        fprintf(stderr, "server: encode worker failed to start\n");
        goto fail;
    }

    /*
     * HTTP listener.
     */
    mg_mgr_init(&server->mgr);

    mg_log_set(config->verbose ? MG_LL_INFO : MG_LL_ERROR);

    char url[128];

    snprintf(url, sizeof(url), "http://%s:%u",
             config->listen, config->http_port);

    server->listener = mg_http_listen(&server->mgr, url,
                                      http_event_handler, server);

    if (server->listener == NULL) {
        fprintf(stderr, "server: cannot listen on %s\n", url);
        goto fail;
    }

    InterfaceInfo interfaces[16];
    size_t interface_count = collect_interfaces(interfaces, 16);

    printf("\ncamstream %s ready [%s]\n", APP_VERSION,
#ifdef USE_LIBPEER
           "libpeer backend - mbedTLS + libsrtp + usrsctp"
#else
           "native backend - OpenSSL + libsrtp2"
#endif
    );

    if (interface_count == 0) {
        printf("open http://localhost:%u/\n", config->http_port);
    }

    for (size_t i = 0; i < interface_count; i++) {
        printf("open http://%s:%u/   (%s)\n",
               interfaces[i].ip, config->http_port, interfaces[i].name);
    }

#ifdef USE_LIBPEER
    printf("udp media: managed internally by libpeer (host candidates, one socket per PeerConnection)\n");
    printf("firewall: allow TCP %u and UDP 50000-50100 (libpeer ephemeral)\n",
           config->http_port);
#else
    printf("udp media ports %u-%u (one per viewer, up to %d)\n",
           config->udp_base_port,
           config->udp_base_port + MAX_RTC_SESSIONS - 1,
           MAX_RTC_SESSIONS);
    printf("firewall: allow TCP %u and UDP %u-%u\n",
           config->http_port,
           config->udp_base_port,
           config->udp_base_port + MAX_RTC_SESSIONS - 1);
#endif
    printf("hint: open the page directly via LAN IP, not through a proxy. "
           "Proxies cannot forward UDP media.\n");
    printf("\n");

    /*
     * Event loop.
     */
    while (!*stop_flag) {
        uint64_t now = now_ms();

        /*
         * 1. Poll every active session socket.
         */
        struct pollfd fds[MAX_RTC_SESSIONS];
        int fd_count = 0;
        int timeout = 10;
        int has_sessions = 0;

        for (size_t i = 0; i < MAX_RTC_SESSIONS; i++) {
            RtcSession *session = server->sessions[i];

            if (session == NULL) {
                continue;
            }

            if (rtc_session_state(session) == RTC_CLOSED) {
                continue;
            }

            has_sessions = 1;

            int session_fd = rtc_session_fd(session);
            if (session_fd >= 0) {
                fds[fd_count].fd = session_fd;
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
            /* Idle: no viewers, save CPU and avoid busy loop */
            timeout = 100;
        }

        if (fd_count > 0) {
            poll(fds, (nfds_t) fd_count, timeout);
        } else {
            poll(NULL, 0, timeout);
        }

        for (int i = 0; i < fd_count; i++) {
            if (!(fds[i].revents & POLLIN)) {
                continue;
            }

            for (size_t s = 0; s < MAX_RTC_SESSIONS; s++) {
                RtcSession *session = server->sessions[s];

                if (session == NULL ||
                    rtc_session_fd(session) != fds[i].fd) {
                    continue;
                }

                uint8_t buffer[2048];
                struct sockaddr_storage source;

                for (;;) {
                    socklen_t source_len = sizeof(source);
                    ssize_t received = recvfrom(fds[i].fd,
                                                buffer, sizeof(buffer),
                                                0,
                                                (struct sockaddr *) &source,
                                                &source_len);

                    if (received <= 0) {
                        break;
                    }

                    rtc_session_on_udp(session, buffer, (size_t) received,
                                       &source);
                }
            }
        }

        /*
         * 2. Mongoose HTTP traffic: web UI and WebRTC signaling.
         */
        mg_mgr_poll(&server->mgr, 0);

        /*
         * 3. Session housekeeping.
         */
        for (size_t i = 0; i < MAX_RTC_SESSIONS; i++) {
            RtcSession *session = server->sessions[i];

            if (session != NULL) {
                rtc_session_tick(session, now);
            }
        }

        /*
         * 4. Fan out encoded access units to every viewer.
         */
        static uint8_t au_buffer[AU_SLOT_CAPACITY];
        AuMeta meta;

        while (au_ring_pop(server->ring,
                           au_buffer, sizeof(au_buffer), &meta)) {
            for (size_t i = 0; i < MAX_RTC_SESSIONS; i++) {
                RtcSession *session = server->sessions[i];

                if (session != NULL &&
                    rtc_session_state(session) == RTC_STREAMING) {
                    rtc_session_send_access_unit(session,
                                                 au_buffer,
                                                 meta.size,
                                                 meta.pts_us,
                                                 meta.is_idr);
                }
            }
        }

        /*
         * 5. Reap closed sessions.
         */
        int live_sessions = 0;

        for (size_t i = 0; i < MAX_RTC_SESSIONS; i++) {
            RtcSession *session = server->sessions[i];

            if (session == NULL) {
                continue;
            }

            if (rtc_session_state(session) == RTC_CLOSED) {
                rtc_session_destroy(session);
                server->sessions[i] = NULL;
            } else {
                live_sessions++;
            }
        }

        atomic_store(&server->rtc_active, live_sessions > 0 ? 1 : 0);
    }

    printf("\nshutting down\n");

    /*
     * Cleanup.
     */
    for (size_t i = 0; i < MAX_RTC_SESSIONS; i++) {
        if (server->sessions[i] != NULL) {
            rtc_session_destroy(server->sessions[i]);
            server->sessions[i] = NULL;
        }
    }

    mg_mgr_free(&server->mgr);

    encoder_worker_stop(server->encoder_worker);
    encoder_worker_destroy(server->encoder_worker);

    h264_encoder_close(server->encoder);

    source_worker_stop(server->source_worker);
    source_worker_destroy(server->source_worker);

    frame_hub_destroy(server->hub);
    video_source_close(server->source);

    free(server);

    dtls_srtp_global_shutdown();

    return 0;

fail:
    if (server->listener != NULL) {
        mg_mgr_free(&server->mgr);
    }

    if (server->encoder_worker != NULL) {
        encoder_worker_stop(server->encoder_worker);
        encoder_worker_destroy(server->encoder_worker);
    }

    if (server->encoder != NULL) {
        h264_encoder_close(server->encoder);
    }

    if (server->source_worker != NULL) {
        source_worker_stop(server->source_worker);
        source_worker_destroy(server->source_worker);
    }

    if (server->ring != NULL) {
        au_ring_destroy(server->ring);
    }

    if (server->hub != NULL) {
        frame_hub_destroy(server->hub);
    }

    if (server->source != NULL) {
        video_source_close(server->source);
    }

    free(server);

    dtls_srtp_global_shutdown();

    return 1;
}
