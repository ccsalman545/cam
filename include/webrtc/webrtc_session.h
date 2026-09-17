/*
 * webrtc_session.h
 *
 * One browser viewer = one RtcSession.
 *
 * A session owns:
 *   - a UDP socket (ICE transport, port allocated from the
 *     configured base port)
 *   - local ICE credentials (validated against every STUN
 *     binding request)
 *   - a DTLS-SRTP engine
 *   - an RTP packetizer with a retransmission cache for NACKs
 *
 * Life cycle:
 *
 *   RTC_NEW        created by POST /rtc/offer, answer sent
 *        |         to the browser
 *        v
 *   RTC_ICE        first valid STUN check received, remote
 *        |         address locked, DTLS starts on first
 *        v         ClientHello
 *   RTC_DTLS       handshake in progress
 *        |
 *        v
 *   RTC_STREAMING  SRTP keys derived, video flows
 *        |
 *        v
 *   RTC_CLOSED     BYE, idle timeout or shutdown
 *
 * Every RTC_* state also falls to RTC_CLOSED on fatal errors.
 */
#ifndef WEBRTC_WEBRTC_SESSION_H
#define WEBRTC_WEBRTC_SESSION_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include "dtls_srtp.h"
#include "rtcp.h"
#include "sdp.h"

typedef struct RtcSession RtcSession;

typedef enum {
    RTC_NEW = 0,
    RTC_ICE,
    RTC_DTLS,
    RTC_STREAMING,
    RTC_CLOSED
} RtcSessionState;

typedef struct {
    uint32_t id;
    uint16_t udp_port;
    const char *advertise_ip;       /* server IP for the c= line and first candidate */
    const char **extra_ips;         /* additional host candidates (local interfaces) */
    size_t extra_ip_count;
    SdpOffer offer;                 /* parsed browser offer */
    const char *remote_sdp;         /* raw offer SDP (required for libpeer backend) */

    /* Server hooks. */
    void *server;
    void (*on_idr_request)(void *server);
    void (*on_closed)(void *server, RtcSession *session);
} RtcSessionConfig;

typedef struct {
    uint64_t packets_sent;
    uint64_t bytes_sent;
    uint32_t pli_received;
    uint32_t nacks_received;
    uint32_t retransmissions;
    uint32_t rtcp_sent;
    uint32_t stun_rx;           /* datagrams classified as STUN */
    uint32_t stun_ok;           /* binding requests we answered */
    uint32_t stun_bad_user;     /* USERNAME or MI rejected (401 sent) */
    uint32_t peer_moved;        /* peer address changed (NAT rebind) */
} RtcSessionStats;

/*
 * Create a session, allocate the UDP socket and build the SDP
 * answer. On success *session_out is set and answer_sdp holds
 * the answer to return in signaling. Returns 0 or -1.
 */
int rtc_session_create(const RtcSessionConfig *config,
                       RtcSession **session_out,
                       char *answer_sdp,
                       size_t answer_capacity,
                       size_t *answer_length);

/* Milliseconds until the next DTLS retransmission timer, -1 none. */
int rtc_session_dtls_timeout_ms(const RtcSession *session);

/* fd for the poll loop. */
int rtc_session_fd(const RtcSession *session);

/* Feed one datagram received on the session fd. */
void rtc_session_on_udp(RtcSession *session,
                        uint8_t *buffer,
                        size_t length,
                        const struct sockaddr_storage *source);

/* Periodic work: DTLS timers, RTCP sender reports, timeouts. */
void rtc_session_tick(RtcSession *session, uint64_t now_ms);

/*
 * Send one H.264 access unit (Annex-B) with this capture
 * timestamp. Returns the number of RTP packets sent.
 */
int rtc_session_send_access_unit(RtcSession *session,
                                 const uint8_t *access_unit,
                                 size_t length,
                                 uint64_t pts_us,
                                 int is_idr);

/* Ask the encoder for a keyframe (rate limited internally). */
void rtc_session_request_idr(RtcSession *session);

RtcSessionState rtc_session_state(const RtcSession *session);

const char *rtc_session_state_name(const RtcSession *session);

void rtc_session_get_stats(const RtcSession *session, RtcSessionStats *out);

uint32_t rtc_session_id(const RtcSession *session);

void rtc_session_close(RtcSession *session);

void rtc_session_destroy(RtcSession *session);

#endif
