#define _POSIX_C_SOURCE 200809L

/*
 * webrtc_session.c
 *
 * Session state machine. See webrtc_session.h for the contract.
 *
 * Failure policy: a fatal protocol error (DTLS handshake failure,
 * unauthenticated checks, an unusable socket) closes this session and
 * nothing else. The HTTP management interface runs in the same loop but
 * on a different socket, so a broken viewer can never take it down.
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
#include "log.h"
#include "rtp_h264.h"

#define SESSION_IDLE_TIMEOUT_MS 15000
#define SESSION_DTLS_WATCHDOG_MS 30000
#define IDR_MIN_INTERVAL_MS 400
#define RTCP_SR_INTERVAL_MS 1000
#define RETX_CACHE_SIZE 512
#define MAX_SEND_ERROR_LOGS 5

struct RtcSession {
    RtcSessionConfig config;
    RtcSessionState state;
    int udp_fd;

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
    unsigned send_errors_logged;

    RtcSessionStats stats;

    /*
     * Retransmission cache, indexed by (sequence % RETX_CACHE_SIZE).
     * Each entry keeps the protected packet exactly as it was sent, so
     * a NACK can be answered with the same bytes (RFC 4588 style), and
     * the sequence number it belongs to, so a NACK for a packet that
     * has already been overwritten is ignored instead of answered with
     * a packet the peer never asked for.
     */
    uint8_t (*retx_data)[RTP_MAX_PACKET + 64];
    uint16_t retx_len[RETX_CACHE_SIZE];
    uint16_t retx_seq[RETX_CACHE_SIZE];
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

/* RFC 3550 A.3 round trip time needs the middle 32 bits of the NTP time. */
static uint32_t ntp_msw(uint64_t wall_us)
{
    uint64_t ntp_seconds = wall_us / 1000000ULL + 2208988800ULL;
    uint32_t ntp_fraction =
        (uint32_t) (((wall_us % 1000000ULL) << 32) / 1000000ULL);

    return (uint32_t) (((ntp_seconds & 0xFFFFULL) << 16) |
                       (ntp_fraction >> 16));
}

static void set_state(RtcSession *session, RtcSessionState state)
{
    if (session->state == state) {
        return;
    }

    session->state = state;

    log_info("rtc", "%08x: state -> %s", session->config.id,
             rtc_session_state_name(session));
}

/* ------------------------------------------------------------------ */
/* Socket helpers                                                      */
/* ------------------------------------------------------------------ */

static socklen_t peer_socklen(const struct sockaddr_storage *peer)
{
    (void) peer;

    /* The media socket is AF_INET; IPv6 peers cannot reach it. */
    return sizeof(struct sockaddr_in);
}

static void format_peer(const struct sockaddr_storage *source,
                        char *ip, size_t ip_size, uint16_t *port)
{
    if (source->ss_family == AF_INET) {
        const struct sockaddr_in *a = (const struct sockaddr_in *) source;

        if (inet_ntop(AF_INET, &a->sin_addr, ip, (socklen_t) ip_size) == NULL) {
            snprintf(ip, ip_size, "?");
        }
        *port = ntohs(a->sin_port);
        return;
    }

    snprintf(ip, ip_size, "?");
    *port = 0;
}

/*
 * Single send path: every RTP, RTCP, STUN and DTLS datagram leaves
 * through here, so a failing socket is counted and reported once
 * instead of silently dropping media.
 */
static int session_sendto(RtcSession *session,
                          const uint8_t *data,
                          size_t length,
                          const struct sockaddr_storage *destination)
{
    ssize_t sent = sendto(session->udp_fd, data, length, 0,
                          (const struct sockaddr *) destination,
                          peer_socklen(destination));

    if (sent == (ssize_t) length) {
        return 0;
    }

    session->stats.send_errors++;

    if (session->send_errors_logged < MAX_SEND_ERROR_LOGS) {
        session->send_errors_logged++;

        if (sent < 0) {
            int error = errno;

            log_warn("rtc", "%08x: sendto(%zu bytes) failed: errno=%d (%s)",
                     session->config.id, length, error, strerror(error));
        } else {
            log_warn("rtc", "%08x: short sendto: %zd of %zu bytes",
                     session->config.id, sent, length);
        }
    } else if (session->send_errors_logged == MAX_SEND_ERROR_LOGS) {
        session->send_errors_logged++;
        log_warn("rtc", "%08x: further send errors are counted in "
                        "/api/stats but not logged", session->config.id);
    }

    return -1;
}

/*
 * Answer a check we will not act on with a STUN error response
 * (RFC 5245 section 16.5) so the peer stops retransmitting instead of
 * concluding that the path is dead.
 */
static void stun_send_error(RtcSession *session,
                            const uint8_t tid[12],
                            uint16_t code,
                            const struct sockaddr_storage *source)
{
    uint8_t response[128];
    size_t response_len = 0;

    if (stun_build_error_response(tid, code, response, sizeof(response),
                                  &response_len) == 0) {
        session_sendto(session, response, response_len, source);
    }
}

/* ------------------------------------------------------------------ */
/* DTLS callbacks                                                      */
/* ------------------------------------------------------------------ */

static void dtls_send_udp(void *user, const uint8_t *packet, size_t len)
{
    RtcSession *session = user;

    if (session->udp_fd < 0 || !session->have_remote) {
        return;
    }

    session_sendto(session, packet, len, &session->remote);
}

static void dtls_on_connected(void *user)
{
    RtcSession *session = user;

    set_state(session, RTC_STREAMING);

    /* A viewer that joins mid-GOP needs a keyframe before it can decode. */
    rtc_session_request_idr(session);

    session->next_sr_ms = now_ms() + RTCP_SR_INTERVAL_MS;
}

static void handle_nacks(RtcSession *session, const RtcpFeedback *feedback)
{
    for (size_t i = 0; i < feedback->nack_seqs; i++) {
        uint16_t seq = feedback->nack_seq[i];
        uint16_t slot = (uint16_t) (seq % RETX_CACHE_SIZE);

        if (session->retx_len[slot] == 0 ||
            session->retx_seq[slot] != seq) {
            continue;
        }

        if (session_sendto(session,
                           session->retx_data[slot],
                           session->retx_len[slot],
                           &session->remote) == 0) {
            session->stats.retransmissions++;
        }
    }
}

static void dtls_on_rtcp(void *user, uint8_t *packet, size_t len)
{
    RtcSession *session = user;

    RtcpFeedback feedback;

    rtcp_parse(packet, len, &feedback);

    if (feedback.bye) {
        log_info("rtc", "%08x: BYE received", session->config.id);
        rtc_session_close(session);
        return;
    }

    if (feedback.pli > 0 || feedback.fir > 0) {
        session->stats.pli_received += (uint32_t) feedback.pli +
                                       (uint32_t) feedback.fir;
        log_debug("rtc", "%08x: keyframe request (pli=%d fir=%d)",
                  session->config.id, feedback.pli, feedback.fir);
        rtc_session_request_idr(session);
    }

    if (feedback.has_rr) {
        session->stats.fraction_lost = feedback.rr_fraction_lost;
        session->stats.jitter = feedback.rr_jitter;
        session->stats.rtt_ms = rtcp_rtt_ms(ntp_msw(wall_us()),
                                            feedback.rr_last_sr,
                                            feedback.rr_delay_since_sr);
    }

    if (feedback.nack_seqs > 0) {
        session->stats.nacks_received += (uint32_t) feedback.nack_seqs;
        handle_nacks(session, &feedback);
    }
}

static void dtls_on_state(void *user, DtlsSrtpState state)
{
    RtcSession *session = user;

    if (state == DTLS_SRTP_HANDSHAKING) {
        set_state(session, RTC_DTLS);
    } else if (state == DTLS_SRTP_FAILED) {
        log_error("rtc", "%08x: DTLS failed: %s", session->config.id,
                  dtls_srtp_failure_reason(session->dtls));
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

    if (len > RTP_MAX_PACKET + 64 || len < RTP_HEADER_SIZE) {
        return;
    }

    uint8_t buffer[RTP_MAX_PACKET + 64];

    memcpy(buffer, packet, len);

    size_t protected_len = len;

    if (dtls_srtp_send_rtp(session->dtls, buffer, &protected_len) != 0) {
        return;
    }

    uint16_t slot = (uint16_t) (sequence % RETX_CACHE_SIZE);

    memcpy(session->retx_data[slot], buffer, protected_len);
    session->retx_len[slot] = (uint16_t) protected_len;
    session->retx_seq[slot] = sequence;

    session->stats.packets_sent++;
    session->stats.bytes_sent += len;
}

/* ------------------------------------------------------------------ */
/* Session API                                                         */
/* ------------------------------------------------------------------ */

static int generate_credentials(char *ufrag, size_t ufrag_size,
                                char *pwd, size_t pwd_size)
{
    unsigned char raw[32];

    /*
     * RAND_bytes fails only when the CSPRNG cannot be seeded, which
     * would break DTLS too. Report it instead of falling back to weak
     * credentials derived from the clock.
     */
    if (RAND_bytes(raw, sizeof(raw)) != 1) {
        log_error("rtc", "RAND_bytes failed while generating ICE credentials");
        return -1;
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

    memset(raw, 0, sizeof(raw));

    return 0;
}

/*
 * Release the resources of a session that was never announced to the
 * server (create failure) or is already closed. Deliberately does not
 * call rtc_session_close(): the caller either owns a session that is
 * already closed, or one that never reached the server's session table,
 * and firing on_closed for it would report a session that never
 * existed.
 */
static void session_free(RtcSession *session)
{
    if (session == NULL) {
        return;
    }

    if (session->udp_fd >= 0) {
        close(session->udp_fd);
    }

    dtls_srtp_session_destroy(session->dtls);
    rtp_h264_destroy(session->rtp);

    free(session->retx_data);
    free(session);
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
    session->stats.rtt_ms = -1;
    snprintf(session->stats.peer, sizeof(session->stats.peer), "-");

    session->retx_data = malloc((size_t) RETX_CACHE_SIZE *
                                (RTP_MAX_PACKET + 64));

    if (session->retx_data == NULL) {
        log_error("rtc", "%08x: retransmission cache allocation failed "
                         "(%zu bytes)", config->id,
                  (size_t) RETX_CACHE_SIZE * (RTP_MAX_PACKET + 64));
        free(session);
        return -1;
    }

    if (generate_credentials(session->local_ufrag,
                             sizeof(session->local_ufrag),
                             session->local_pwd,
                             sizeof(session->local_pwd)) != 0) {
        session_free(session);
        return -1;
    }

    uint16_t bound_port = 0;

    session->udp_fd = udp_socket_create(config->udp_port, &bound_port);

    if (session->udp_fd < 0) {
        log_error("rtc", "%08x: UDP port %u unavailable: errno=%d (%s)",
                  config->id, config->udp_port, errno, strerror(errno));
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
        log_error("rtc", "%08x: DTLS session setup failed", config->id);
        session_free(session);
        return -1;
    }

    dtls_srtp_set_expected_fingerprint(session->dtls,
                                       config->offer.fingerprint);

    session->rtp = rtp_h264_create();

    if (session->rtp == NULL) {
        log_error("rtc", "%08x: packetizer allocation failed", config->id);
        session_free(session);
        return -1;
    }

    uint32_t ssrc = 0;

    if (RAND_bytes((unsigned char *) &ssrc, sizeof(ssrc)) != 1 || ssrc == 0) {
        log_error("rtc", "%08x: SSRC generation failed", config->id);
        session_free(session);
        return -1;
    }

    rtp_h264_reset(session->rtp, ssrc,
                   (uint32_t) config->offer.h264_payload_type);

    session->created_ms = now_ms();
    session->last_rx_ms = session->created_ms;

    SdpAnswerConfig answer_config;

    memset(&answer_config, 0, sizeof(answer_config));
    answer_config.fingerprint = dtls_srtp_local_fingerprint();
    answer_config.ice_ufrag = session->local_ufrag;
    answer_config.ice_pwd = session->local_pwd;
    answer_config.udp_port = bound_port;
    answer_config.ssrc = ssrc;
    answer_config.candidate_count = config->candidate_count;

    for (size_t i = 0; i < config->candidate_count &&
                       i < SDP_MAX_CANDIDATES; i++) {
        answer_config.candidate_ips[i] = config->candidate_ips[i];
    }

    size_t built = sdp_build_answer(&config->offer, &answer_config,
                                    answer_sdp, answer_capacity);

    if (built == 0) {
        log_error("rtc", "%08x: SDP answer does not fit in %zu bytes",
                  config->id, answer_capacity);
        session_free(session);
        return -1;
    }

    *answer_length = built;
    *session_out = session;

    log_info("rtc", "%08x: created, UDP %u, payload type %d, %zu candidates",
             config->id, bound_port, config->offer.h264_payload_type,
             config->candidate_count);

    return 0;
}

int rtc_session_fd(const RtcSession *session)
{
    return session != NULL ? session->udp_fd : -1;
}

static void handle_stun(RtcSession *session,
                        const uint8_t *buffer,
                        size_t length,
                        const struct sockaddr_storage *source,
                        const char *ip,
                        uint16_t port)
{
    uint8_t tid[12] = { 0 };
    char username[160] = "";

    session->stats.stun_rx++;
    session->last_rx_ms = now_ms();

    if (stun_is_binding_indication(buffer, length)) {
        /* Keepalive: no transaction to answer, just refreshed liveness. */
        return;
    }

    if (!stun_is_binding_request(buffer, length, tid)) {
        log_debug("ice", "%08x: STUN message is not a binding request "
                         "(len %zu from %s:%u)",
                  session->config.id, length, ip, port);
        return;
    }

    stun_copy_username(buffer, length, username, sizeof(username));

    /*
     * RFC 5389 section 7.2 / RFC 5245: verify MESSAGE-INTEGRITY before
     * acting on a check. The ice-ufrag is public in the SDP answer, so
     * without this any host on the LAN could claim the peer slot.
     * Unauthenticated checks are answered with 401 (RFC 5245 section
     * 16.5) rather than dropped silently.
     */
    if (!stun_verify_mi(buffer, length, session->local_pwd)) {
        session->stats.stun_bad++;
        log_warn("ice", "%08x: STUN MESSAGE-INTEGRITY invalid from %s:%u "
                        "(user '%s'); answering 401",
                 session->config.id, ip, port,
                 username[0] ? username : "(missing)");
        stun_send_error(session, tid, 401, source);
        return;
    }

    if (!stun_username_matches(buffer, length, session->local_ufrag)) {
        session->stats.stun_bad++;
        log_warn("ice", "%08x: STUN USERNAME mismatch from %s:%u (got '%s', "
                        "expected '%s:<peer-ufrag>'); answering 401",
                 session->config.id, ip, port,
                 username[0] ? username : "(missing)", session->local_ufrag);
        stun_send_error(session, tid, 401, source);
        return;
    }

    session->stats.stun_ok++;

    /*
     * ICE-lite (RFC 8445 section 6.2) tracks the source of the latest
     * authenticated check. A client whose NAT rebinds sends its
     * keepalives, DTLS and RTCP from a new source address; pinning the
     * first address would keep sending media into a dead 5-tuple.
     */
    if (!session->have_remote) {
        session->remote = *source;
        session->have_remote = 1;
        snprintf(session->stats.peer, sizeof(session->stats.peer),
                 "%s:%u", ip, port);

        log_info("ice", "%08x: ICE validated (%s:%u) user=%s",
                 session->config.id, ip, port, username);
    } else {
        char old_ip[INET_ADDRSTRLEN];
        uint16_t old_port = 0;

        format_peer(&session->remote, old_ip, sizeof(old_ip), &old_port);

        if (strcmp(old_ip, ip) != 0 || old_port != port) {
            log_warn("ice", "%08x: peer moved %s:%u -> %s:%u (NAT rebind), "
                            "media follows the latest valid check",
                     session->config.id, old_ip, old_port, ip, port);
            session->remote = *source;
            session->stats.peer_moved++;
            snprintf(session->stats.peer, sizeof(session->stats.peer),
                     "%s:%u", ip, port);
        }
    }

    if (session->state == RTC_NEW) {
        set_state(session, RTC_ICE);
    }

    uint8_t response[128];
    size_t response_len = 0;

    if (stun_build_binding_response(session->local_pwd,
                                    buffer, length, source,
                                    response, sizeof(response),
                                    &response_len) == 0) {
        session_sendto(session, response, response_len, source);
    } else {
        log_warn("ice", "%08x: could not build a binding response", 
                 session->config.id);
    }
}

void rtc_session_on_udp(RtcSession *session,
                        const uint8_t *buffer,
                        size_t length,
                        const struct sockaddr_storage *source)
{
    if (session == NULL || session->state == RTC_CLOSED) {
        return;
    }

    session->stats.datagrams_rx++;

    char ip[INET_ADDRSTRLEN];
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

        log_debug("rtc", "%08x: UDP %s %zu bytes from %s:%u",
                  session->config.id, name, length, ip, port);
        session->datagrams_logged++;
    }

    switch (kind) {
    case RTC_PKT_STUN:
        handle_stun(session, buffer, length, source, ip, port);
        break;

    case RTC_PKT_DTLS:
        if (!session->have_remote) {
            log_debug("rtc", "%08x: DTLS from %s:%u before ICE validation",
                      session->config.id, ip, port);
            break;
        }
        session->last_rx_ms = now_ms();
        dtls_srtp_on_udp(session->dtls, buffer, length);
        break;

    case RTC_PKT_RTP: {
        /*
         * The second byte decides RTP versus RTCP: RTCP payload types
         * are 192 to 223, which the 0x7F mask folds to 64 to 95. This
         * server is sendonly, so inbound RTP is dropped and only
         * feedback (RTCP) is processed.
         */
        uint8_t pt = buffer[1] & 0x7F;

        if (pt >= 64 && pt <= 95 && session->state == RTC_STREAMING) {
            session->last_rx_ms = now_ms();

            size_t len = length;
            uint8_t plain[1500];

            if (len > sizeof(plain)) {
                break;
            }

            memcpy(plain, buffer, len);

            if (dtls_srtp_unprotect_rtcp(session->dtls, plain, &len) == 0) {
                dtls_on_rtcp(session, plain, len);
            } else {
                log_debug("rtc", "%08x: SRTCP authentication failed "
                                  "(%zu bytes from %s:%u)",
                          session->config.id, length, ip, port);
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
     * Idle timeout. last_rx_ms starts at creation, so a session whose
     * browser vanished before the first check is reaped as well.
     * A timestamp ahead of 'now' means "fresh"; treat the underflow as
     * zero rather than as a huge age.
     */
    uint64_t idle_ms = now >= session->last_rx_ms ? now - session->last_rx_ms : 0;

    if (idle_ms > SESSION_IDLE_TIMEOUT_MS) {
        log_info("rtc", "%08x: idle timeout after %llums (stun_rx=%u ok=%u "
                        "bad=%u dtls=%s)",
                 session->config.id, (unsigned long long) idle_ms,
                 session->stats.stun_rx, session->stats.stun_ok,
                 session->stats.stun_bad,
                 rtc_session_state_name(session));
        rtc_session_close(session);
        return;
    }

    uint64_t age_ms = now >= session->created_ms ? now - session->created_ms : 0;

    if (session->state == RTC_ICE && age_ms > SESSION_DTLS_WATCHDOG_MS) {
        log_warn("rtc", "%08x: ICE validated but no DTLS ClientHello within "
                        "%ums (wrong fingerprint or one way UDP?)",
                 session->config.id, SESSION_DTLS_WATCHDOG_MS);
        rtc_session_close(session);
        return;
    }

    dtls_srtp_tick(session->dtls);

    if (session->state == RTC_STREAMING && now >= session->next_sr_ms) {
        /*
         * srtp_protect_rtcp() expands the 32 bit RTCP header to the 64
         * bit SRTCP header (+4) and appends a 10 byte auth tag, so the
         * buffer needs 14 bytes of headroom beyond the report.
         */
        uint8_t sr[RTCP_SR_SIZE + 16];

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
    if (session == NULL || session->state != RTC_STREAMING) {
        return 0;
    }

    if (is_idr) {
        session->idr_requested = 0;
    }

    RtpSendContext ctx = {
        .session = session
    };

    int packets = rtp_h264_packetize(session->rtp,
                                     access_unit, length, pts_us,
                                     rtp_packet_sink, &ctx);

    if (!session->streaming_announced && packets > 0) {
        session->streaming_announced = 1;
        log_info("rtc", "%08x: streaming video", session->config.id);
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

    /*
     * Mark the session closed BEFORE tearing down DTLS: dtls_srtp_close()
     * fires the state callback, which calls back into this function. With
     * the state still open, that re-entrant call would run the whole
     * close sequence and the on_closed hook a second time.
     */
    set_state(session, RTC_CLOSED);

    dtls_srtp_close(session->dtls);

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

    session_free(session);
}

int rtc_session_dtls_timeout_ms(const RtcSession *session)
{
    if (session == NULL) {
        return -1;
    }

    return dtls_srtp_next_timeout_ms(session->dtls);
}
