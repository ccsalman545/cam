#define _POSIX_C_SOURCE 200809L

/*
 * webrtc_session.c
 *
 * Session state machine, see webrtc_session.h.
 */
#include "webrtc_session.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <openssl/rand.h>

#include "ice_lite.h"
#include "rtp_h264.h"

#define SESSION_IDLE_TIMEOUT_MS 15000
#define SESSION_DTLS_WATCHDOG_MS 30000
#define IDR_MIN_INTERVAL_MS 400
#define RTCP_SR_INTERVAL_MS 1000
#define RETX_CACHE_SIZE 512

struct RtcSession {
    RtcSessionConfig config;
    RtcSessionState state;
    int udp_fd;
    uint16_t udp_port;

    char local_ufrag[16];
    char local_pwd[44];

    struct sockaddr_storage remote;
    int have_remote;

    DtlsSrtp *dtls;
    RtpH264 *rtp;

    uint64_t created_ms;
    uint64_t last_rx_ms;
    uint64_t last_idr_ms;
    uint64_t next_sr_ms;
    int idr_requested;
    int streaming_announced;
    unsigned datagrams_logged;

    RtcSessionStats stats;

    /*
     * Retransmission cache: rings indexed by sequence number.
     * Entries store the protected packet exactly as it was
     * sent, which is valid RFC 4588 style retransmission.
     */
    uint8_t (*retx_data)[RTP_MAX_PACKET + 64];
    uint16_t retx_len[RETX_CACHE_SIZE];
};

static uint64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t) ts.tv_sec * 1000ULL +
           (uint64_t) ts.tv_nsec / 1000000ULL;
}

static uint64_t wall_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);

    return (uint64_t) ts.tv_sec * 1000000ULL +
           (uint64_t) ts.tv_nsec / 1000ULL;
}

static void set_state(RtcSession *session, RtcSessionState state)
{
    if (session->state == state) {
        return;
    }

    session->state = state;

    printf("rtc %08x: state -> %s\n",
           session->config.id, rtc_session_state_name(session));
}

/* ------------------------------------------------------------------ */
/* DTLS callbacks                                                      */
/* ------------------------------------------------------------------ */

static socklen_t peer_socklen(const struct sockaddr_storage *peer)
{
    if (peer->ss_family == AF_INET) {
        return sizeof(struct sockaddr_in);
    }

    if (peer->ss_family == AF_INET6) {
        return sizeof(struct sockaddr_in6);
    }

    return sizeof(struct sockaddr_storage);
}

static void format_peer(const struct sockaddr_storage *source,
                        char *ip, size_t ip_size, uint16_t *port)
{
    ip[0] = '?';
    ip[1] = 0;
    *port = 0;

    if (source->ss_family == AF_INET) {
        const struct sockaddr_in *a = (const struct sockaddr_in *) source;
        inet_ntop(AF_INET, &a->sin_addr, ip, (socklen_t) ip_size);
        *port = ntohs(a->sin_port);
    } else if (source->ss_family == AF_INET6) {
        const struct sockaddr_in6 *a = (const struct sockaddr_in6 *) source;
        inet_ntop(AF_INET6, &a->sin6_addr, ip, (socklen_t) ip_size);
        *port = ntohs(a->sin6_port);
    }
}

static void dtls_send_udp(void *user, const uint8_t *packet, size_t len)
{
    RtcSession *session = user;

    if (session->udp_fd < 0 || !session->have_remote) {
        return;
    }

    ssize_t sent = sendto(session->udp_fd, packet, len, 0,
                          (struct sockaddr *) &session->remote,
                          peer_socklen(&session->remote));

    (void) sent;
}

static void dtls_on_connected(void *user)
{
    RtcSession *session = user;

    set_state(session, RTC_STREAMING);

    /*
     * A fresh viewer must receive a keyframe before anything
     * else can be decoded.
     */
    rtc_session_request_idr(session);

    session->next_sr_ms = now_ms() + RTCP_SR_INTERVAL_MS;
}

static void dtls_on_rtcp(void *user, uint8_t *packet, size_t len)
{
    RtcSession *session = user;

    RtcpFeedback feedback;

    rtcp_parse(packet, len, &feedback);

    if (feedback.bye) {
        printf("rtc %08x: BYE received\n", session->config.id);
        rtc_session_close(session);
        return;
    }

    if (feedback.pli > 0 || feedback.fir > 0) {
        session->stats.pli_received += (uint32_t) feedback.pli +
                                       (uint32_t) feedback.fir;
        rtc_session_request_idr(session);
    }

    if (feedback.nack_seqs > 0) {
        session->stats.nacks_received += (uint32_t) feedback.nack_seqs;

        for (size_t i = 0; i < feedback.nack_seqs; i++) {
            uint16_t seq = feedback.nack_seq[i];
            uint16_t slot = seq % RETX_CACHE_SIZE;

            if (session->retx_len[slot] > 0) {
                dtls_send_udp(session,
                              session->retx_data[slot],
                              session->retx_len[slot]);
                session->stats.retransmissions++;
            }
        }
    }
}

static void dtls_on_state(void *user, DtlsSrtpState state)
{
    RtcSession *session = user;

    if (state == DTLS_SRTP_HANDSHAKING) {
        set_state(session, RTC_DTLS);
    } else if (state == DTLS_SRTP_FAILED) {
        printf("rtc %08x: DTLS handshake failed\n", session->config.id);
        rtc_session_close(session);
    } else if (state == DTLS_SRTP_CLOSED) {
        rtc_session_close(session);
    }
}

/* ------------------------------------------------------------------ */
/* RTP send path                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    RtcSession *session;
    int ok;
} RtpSendContext;

static void rtp_packet_sink(void *user,
                            const uint8_t *packet,
                            size_t len,
                            int marker,
                            uint16_t sequence)
{
    (void) marker;

    RtpSendContext *ctx = user;
    RtcSession *session = ctx->session;

    if (len > RTP_MAX_PACKET + 64) {
        ctx->ok = 0;
        return;
    }

    uint8_t buffer[RTP_MAX_PACKET + 64];

    memcpy(buffer, packet, len);

    size_t protected_len = len;

    if (dtls_srtp_send_rtp(session->dtls, buffer, &protected_len) != 0) {
        ctx->ok = 0;
        return;
    }

    /*
     * Cache the protected packet for NACK retransmission.
     */
    uint16_t slot = sequence % RETX_CACHE_SIZE;

    memcpy(session->retx_data[slot], buffer, protected_len);
    session->retx_len[slot] = (uint16_t) protected_len;

    session->stats.packets_sent++;
    session->stats.bytes_sent += len;
}

/* ------------------------------------------------------------------ */
/* Session API                                                         */
/* ------------------------------------------------------------------ */

static void generate_credentials(char *ufrag, size_t ufrag_size,
                                 char *pwd, size_t pwd_size)
{
    unsigned char raw[32];

    if (RAND_bytes(raw, sizeof(raw)) != 1) {
        /* Fallback: try /dev/urandom, then time-based entropy */
        FILE *f = fopen("/dev/urandom", "rb");
        if (f != NULL) {
            size_t got = fread(raw, 1, sizeof(raw), f);
            fclose(f);
            if (got != sizeof(raw)) {
                /* partial read, fill rest with time */
                uint64_t t = (uint64_t) time(NULL) ^ (uint64_t) clock();
                for (size_t i = got; i < sizeof(raw); i++) {
                    raw[i] ^= (unsigned char) (t >> (i % 8));
                }
            }
        } else {
            /* Last resort: mix time and pid */
            uint64_t seed = (uint64_t) time(NULL) ^ (uint64_t) getpid() ^ (uint64_t) clock();
            for (size_t i = 0; i < sizeof(raw); i++) {
                seed = seed * 6364136223846793005ULL + 1;
                raw[i] = (unsigned char) (seed >> 24);
            }
        }
    }

    static const char alphabet[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

    size_t pos = 0;

    for (size_t i = 0; i < 8 && pos + 1 < ufrag_size; i++) {
        ufrag[pos++] = alphabet[raw[i] % 52];
    }
    ufrag[pos] = 0;

    pos = 0;
    for (size_t i = 0; i < 24 && pos + 1 < pwd_size; i++) {
        pwd[pos++] = alphabet[raw[8 + i] % 52];
    }
    pwd[pos] = 0;

    /* Wipe raw material */
    memset(raw, 0, sizeof(raw));
}

int rtc_session_create(const RtcSessionConfig *config,
                       RtcSession **session_out,
                       char *answer_sdp,
                       size_t answer_capacity,
                       size_t *answer_length)
{
    *session_out = NULL;

    RtcSession *session = calloc(1, sizeof(*session));

    if (session == NULL) {
        return -1;
    }

    session->config = *config;
    session->udp_fd = -1;
    session->state = RTC_NEW;

    session->retx_data = malloc((size_t) RETX_CACHE_SIZE *
                                (RTP_MAX_PACKET + 64));

    if (session->retx_data == NULL) {
        free(session);
        return -1;
    }

    generate_credentials(session->local_ufrag, sizeof(session->local_ufrag),
                         session->local_pwd, sizeof(session->local_pwd));

    session->udp_fd = udp_socket_create(config->udp_port, &session->udp_port);

    if (session->udp_fd < 0) {
        fprintf(stderr, "rtc %08x: UDP port %u unavailable\n",
                config->id, config->udp_port);
        free(session->retx_data);
        free(session);
        return -1;
    }

    DtlsSrtpCallbacks dtls_callbacks = {
        .user = session,
        .send_udp = dtls_send_udp,
        .on_connected = dtls_on_connected,
        .on_rtcp = dtls_on_rtcp,
        .on_state = dtls_on_state
    };

    session->dtls = dtls_srtp_session_create(&dtls_callbacks);

    if (session->dtls == NULL) {
        close(session->udp_fd);
        free(session->retx_data);
        free(session);
        return -1;
    }

    dtls_srtp_set_expected_fingerprint(session->dtls,
                                       config->offer.fingerprint);

    session->rtp = rtp_h264_create();

    uint32_t ssrc = 0;

    RAND_bytes((unsigned char *) &ssrc, sizeof(ssrc));
    if (ssrc == 0) {
        ssrc = 0x1234ABCD;
    }

    rtp_h264_reset(session->rtp, ssrc,
                   (uint32_t) config->offer.h264_payload_type);

    session->created_ms = now_ms();
    session->last_rx_ms = session->created_ms;

    size_t built = 0;
    if (config->extra_ips != NULL && config->extra_ip_count > 0) {
        built = sdp_build_answer_multi(&config->offer,
                                       dtls_srtp_local_fingerprint(),
                                       session->local_ufrag,
                                       session->local_pwd,
                                       config->advertise_ip,
                                       config->extra_ips,
                                       config->extra_ip_count,
                                       session->udp_port,
                                       ssrc,
                                       answer_sdp,
                                       answer_capacity);
    } else {
        built = sdp_build_answer(&config->offer,
                                 dtls_srtp_local_fingerprint(),
                                 session->local_ufrag,
                                 session->local_pwd,
                                 config->advertise_ip,
                                 session->udp_port,
                                 ssrc,
                                 answer_sdp,
                                 answer_capacity);
    }

    if (built == 0) {
        fprintf(stderr, "rtc %08x: SDP answer overflow\n", config->id);
        rtc_session_destroy(session);
        return -1;
    }

    *answer_length = built;

    /* Clear dangling extra IP pointers that were on the caller's stack.
     * The SDP has already been built, we don't need them anymore. */
    session->config.extra_ips = NULL;
    session->config.extra_ip_count = 0;

    *session_out = session;

    printf("rtc %08x: created, UDP %u, ice-ufrag %s, payload type %d, candidates %zu\n",
           config->id, session->udp_port, session->local_ufrag,
           config->offer.h264_payload_type,
           1 + config->extra_ip_count);

    return 0;
}

int rtc_session_fd(const RtcSession *session)
{
    return session != NULL ? session->udp_fd : -1;
}

void rtc_session_on_udp(RtcSession *session,
                        uint8_t *buffer,
                        size_t length,
                        const struct sockaddr_storage *source)
{
    if (session == NULL || session->state == RTC_CLOSED) {
        return;
    }

    session->last_rx_ms = now_ms();

    char ip[INET6_ADDRSTRLEN];
    uint16_t port = 0;
    format_peer(source, ip, sizeof(ip), &port);

    RtcPacketClass kind = rtc_classify_packet(buffer, length);

    if (session->datagrams_logged < 8) {
        const char *name = "other";

        if (kind == RTC_PKT_STUN) {
            name = "STUN";
        } else if (kind == RTC_PKT_DTLS) {
            name = "DTLS";
        } else if (kind == RTC_PKT_RTP) {
            name = "RTP/RTCP";
        }

        printf("rtc %08x: UDP %s %zu bytes from %s:%u\n",
               session->config.id, name, length, ip, port);
        session->datagrams_logged++;
    }

    switch (kind) {

    case RTC_PKT_STUN: {
        uint8_t tid[12];
        char username[160] = "";

        session->stats.stun_rx++;
        stun_copy_username(buffer, length, username, sizeof(username));

        if (!stun_is_binding_request(buffer, length, tid)) {
            printf("rtc %08x: STUN ignored (not a binding request) "
                   "from %s:%u len=%zu\n",
                   session->config.id, ip, port, length);
            break;
        }

        if (!stun_username_matches(buffer, length, session->local_ufrag)) {
            session->stats.stun_bad_user++;
            printf("rtc %08x: STUN username mismatch from %s:%u "
                   "(got '%s', want '%s:<peer-ufrag>')\n",
                   session->config.id, ip, port,
                   username[0] ? username : "(missing)",
                   session->local_ufrag);
            break;
        }

        session->stats.stun_ok++;

        /*
         * Valid connectivity check: lock the peer address
         * and answer.
         */
        if (!session->have_remote) {
            session->remote = *source;
            session->have_remote = 1;

            printf("rtc %08x: ICE validated (%s:%u) username=%s\n",
                   session->config.id, ip, port, username);
        }

        if (session->state == RTC_NEW) {
            set_state(session, RTC_ICE);
        }

        uint8_t response[128];
        size_t response_len = 0;

        if (stun_build_binding_response(session->local_pwd,
                                        buffer, length,
                                        source,
                                        response, sizeof(response),
                                        &response_len) == 0) {
            sendto(session->udp_fd, response, response_len, 0,
                   (struct sockaddr *) source,
                   peer_socklen(source));
        }
        break;
    }

    case RTC_PKT_DTLS:
        if (session->have_remote) {
            dtls_srtp_on_udp(session->dtls, buffer, length);
        } else {
            printf("rtc %08x: DTLS before ICE from %s:%u, %zu bytes\n",
                   session->config.id, ip, port, length);
        }
        break;

    case RTC_PKT_RTP: {
        /*
         * The second byte decides RTP versus RTCP. RTCP
         * payload types are 192 to 223, which the 0x7F mask
         * folds into 64 to 95. We are sendonly: inbound RTP is
         * dropped, inbound RTCP is processed.
         */
        uint8_t pt = buffer[1] & 0x7F;

        if (pt >= 64 && pt <= 95 && session->state == RTC_STREAMING) {
            size_t len = length;

            if (dtls_srtp_unprotect_rtcp(session->dtls, buffer, &len) == 0) {
                dtls_on_rtcp(session, buffer, len);
            }
        }
        break;
    }

    default:
        break;
    }
}

void rtc_session_tick(RtcSession *session, uint64_t now)
{
    if (session == NULL || session->state == RTC_CLOSED) {
        return;
    }

    /*
     * Idle timeout. last_rx_ms starts at creation, so a session
     * that never receives even the first STUN check is reaped
     * too (otherwise a vanished browser would hold the slot
     * forever).
     */
    if (now - session->last_rx_ms > SESSION_IDLE_TIMEOUT_MS) {
        printf("rtc %08x: idle timeout (stun_rx=%u stun_ok=%u stun_bad_user=%u)\n",
               session->config.id,
               session->stats.stun_rx,
               session->stats.stun_ok,
               session->stats.stun_bad_user);
        rtc_session_close(session);
        return;
    }

    /*
     * Handshake watchdog.
     */
    if (session->state == RTC_ICE &&
        now - session->created_ms > SESSION_DTLS_WATCHDOG_MS) {
        printf("rtc %08x: DTLS never started\n", session->config.id);
        rtc_session_close(session);
        return;
    }

    dtls_srtp_tick(session->dtls);

    /*
     * Sender reports.
     */
    if (session->state == RTC_STREAMING && now >= session->next_sr_ms) {
        uint8_t sr[RTCP_SR_SIZE];

        rtcp_build_sender_report(sr,
                                 rtp_h264_ssrc(session->rtp),
                                 wall_us(),
                                 rtp_h264_last_timestamp(session->rtp),
                                 rtp_h264_packet_count(session->rtp),
                                 rtp_h264_octet_count(session->rtp));

        size_t len = RTCP_SR_SIZE;

        if (dtls_srtp_send_rtcp(session->dtls, sr, &len) == 0) {
            session->stats.rtcp_sent++;
        }

        session->next_sr_ms = now + RTCP_SR_INTERVAL_MS;
    }
}

int rtc_session_send_access_unit(RtcSession *session,
                                 const uint8_t *access_unit,
                                 size_t length,
                                 uint64_t pts_us,
                                 int is_idr)
{
    if (session == NULL) {
        return 0;
    }

    if (is_idr && session->idr_requested) {
        session->idr_requested = 0;
    }

    if (session->state != RTC_STREAMING) {
        return 0;
    }

    RtpSendContext ctx = {
        .session = session,
        .ok = 1
    };

    int packets = rtp_h264_packetize(session->rtp,
                                     access_unit, length, pts_us,
                                     rtp_packet_sink, &ctx);

    if (!session->streaming_announced && packets > 0) {
        session->streaming_announced = 1;
        printf("rtc %08x: streaming video\n", session->config.id);
    }

    return packets;
}

void rtc_session_request_idr(RtcSession *session)
{
    if (session == NULL || session->config.on_idr_request == NULL) {
        return;
    }

    uint64_t now = now_ms();

    if (session->last_idr_ms != 0 &&
        now - session->last_idr_ms < IDR_MIN_INTERVAL_MS) {
        return;
    }

    session->last_idr_ms = now;
    session->idr_requested = 1;

    session->config.on_idr_request(session->config.server);
}

RtcSessionState rtc_session_state(const RtcSession *session)
{
    return session != NULL ? session->state : RTC_CLOSED;
}

const char *rtc_session_state_name(const RtcSession *session)
{
    if (session == NULL) {
        return "closed";
    }

    switch (session->state) {
    case RTC_NEW:       return "new";
    case RTC_ICE:       return "ice";
    case RTC_DTLS:      return "dtls";
    case RTC_STREAMING: return "streaming";
    default:            return "closed";
    }
}

void rtc_session_get_stats(const RtcSession *session, RtcSessionStats *out)
{
    if (session != NULL && out != NULL) {
        *out = session->stats;
    }
}

uint32_t rtc_session_id(const RtcSession *session)
{
    return session != NULL ? session->config.id : 0;
}

void rtc_session_close(RtcSession *session)
{
    if (session == NULL || session->state == RTC_CLOSED) {
        return;
    }

    dtls_srtp_close(session->dtls);

    set_state(session, RTC_CLOSED);

    if (session->config.on_closed != NULL) {
        session->config.on_closed(session->config.server, session);
    }
}

void rtc_session_destroy(RtcSession *session)
{
    if (session == NULL) {
        return;
    }

    if (session->state != RTC_CLOSED) {
        rtc_session_close(session);
    }

    if (session->udp_fd >= 0) {
        close(session->udp_fd);
    }

    dtls_srtp_session_destroy(session->dtls);
    rtp_h264_destroy(session->rtp);

    free(session->retx_data);
    free(session);
}

int rtc_session_dtls_timeout_ms(const RtcSession *session)
{
    if (session == NULL) {
        return -1;
    }

    return dtls_srtp_next_timeout_ms(session->dtls);
}
