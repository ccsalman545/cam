/*
 * webrtc_session.h
 *
 * One browser viewer = one RtcSession.
 *
 * A session owns:
 *   - a UDP socket (ICE transport, one port from the configured base)
 *   - local ICE credentials (validated against every STUN check)
 *   - a DTLS-SRTP engine
 *   - an RTP packetizer plus a retransmission cache for NACKs
 *
 * Life cycle:
 *
 *   RTC_NEW        created by POST /api/webrtc/offer, answer returned
 *        |
 *        v
 *   RTC_ICE        first authenticated STUN check, peer address locked
 *        |
 *        v
 *   RTC_DTLS       DTLS handshake in progress
 *        |
 *        v
 *   RTC_STREAMING  SRTP keys derived, media flows
 *        |
 *        v
 *   RTC_CLOSED     BYE, idle timeout, DTLS failure or shutdown
 *
 * Every state also falls to RTC_CLOSED on a fatal error; a closed
 * session is destroyed by the server loop, not by itself.
 *
 * Threading: a session is only ever touched from the thread that runs
 * the server loop (sockets, state, statistics). The encoder and source
 * threads never see session objects; they communicate through the
 * access unit ring and atomic flags.
 */
#ifndef WEBRTC_WEBRTC_SESSION_H
#define WEBRTC_WEBRTC_SESSION_H

#include <netinet/in.h>
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
    /* Host candidates offered in the SDP answer; [0] is the c= address. */
    char candidate_ips[SDP_MAX_CANDIDATES][INET_ADDRSTRLEN];
    size_t candidate_count;
    SdpOffer offer;                 /* parsed browser offer */
    char signaling_peer[48];        /* HTTP client that posted the offer */

    /* Server hooks. */
    void *server;
    void (*on_idr_request)(void *server);
    void (*on_closed)(void *server, RtcSession *session);
} RtcSessionConfig;

typedef struct {
    uint64_t datagrams_rx;          /* every datagram on the media socket */
    uint64_t packets_sent;          /* RTP packets handed to the socket */
    uint64_t bytes_sent;            /* RTP payload bytes */
    uint64_t retransmissions;       /* NACK answered from the cache */
    uint64_t send_errors;           /* sendto failures, any errno */

    uint32_t pli_received;          /* PLI and FIR combined */
    uint32_t nacks_received;        /* requested sequence numbers */
    uint32_t rtcp_sent;             /* sender reports */
    uint32_t stun_rx;
    uint32_t stun_ok;               /* authenticated checks answered */
    uint32_t stun_bad;              /* rejected: username or integrity */
    uint32_t peer_moved;            /* NAT rebinds observed */

    uint64_t frames_sent;           /* access units packetized and sent */
    uint64_t keyframes_sent;        /* of which IDR */
    uint64_t frames_held;           /* withheld while waiting for an IDR */
    uint64_t last_media_ms;         /* monotonic ms of the last sent AU, 0 = never */
    uint32_t rr_received;           /* receiver report blocks about our SSRC */
    uint64_t last_rr_ms;            /* monotonic ms of the last one, 0 = never */
    int waiting_keyframe;           /* 1 until the first IDR went out */

    int rtt_ms;                     /* -1 until the peer reports on an SR */
    uint8_t fraction_lost;          /* 0..255, from the peer's last RR */
    uint32_t jitter;                /* RTP clock units, from the last RR */

    char peer[32];                  /* ICE peer "192.168.1.20:53124" or "-" */
    char signaling_peer[48];        /* HTTP client address of the offer */
    uint8_t payload_type;           /* negotiated H.264 payload type */
    char profile_level_id[8];       /* answered profile-level-id, hex */
} RtcSessionStats;

/*
 * Create a session: allocate the UDP socket, set up DTLS and build the
 * SDP answer. Returns 0 on success with *session_out and the answer
 * text in answer_sdp.
 */
int rtc_session_create(const RtcSessionConfig *config,
                       RtcSession **session_out,
                       char *answer_sdp,
                       size_t answer_capacity,
                       size_t *answer_length);

/* Milliseconds until the next DTLS retransmission, -1 when none. */
int rtc_session_dtls_timeout_ms(const RtcSession *session);

/* Media socket for the poll loop. */
int rtc_session_fd(const RtcSession *session);

/* Feed one datagram received on the session socket. */
void rtc_session_on_udp(RtcSession *session,
                        const uint8_t *buffer,
                        size_t length,
                        const struct sockaddr_storage *source);

/* Periodic work: DTLS timers, sender reports, idle timeouts. */
void rtc_session_tick(RtcSession *session, uint64_t now_ms);

/*
 * Send one H.264 access unit (Annex-B, capture timestamp in
 * microseconds). Returns the number of RTP packets sent, 0 when the
 * session is not streaming yet.
 */
int rtc_session_send_access_unit(RtcSession *session,
                                 const uint8_t *access_unit,
                                 size_t length,
                                 uint64_t pts_us,
                                 int is_idr);

/* Ask the encoder for a keyframe. Rate limited per session. */
void rtc_session_request_idr(RtcSession *session);

RtcSessionState rtc_session_state(const RtcSession *session);

const char *rtc_session_state_name(const RtcSession *session);

void rtc_session_get_stats(const RtcSession *session, RtcSessionStats *out);

uint32_t rtc_session_id(const RtcSession *session);

/* Stop the session; the server loop destroys it on the next tick. */
void rtc_session_close(RtcSession *session);

void rtc_session_destroy(RtcSession *session);

#endif
