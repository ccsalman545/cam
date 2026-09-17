/*
 * dtls_srtp.h
 *
 * DTLS 1.2 server handshake with SRTP key export (RFC 5764)
 * on top of a UDP socket owned by the caller.
 *
 * Integration model:
 *   - The caller receives raw datagrams classified as DTLS
 *     and forwards them with dtls_srtp_on_udp().
 *   - A custom BIO queues inbound datagrams and routes
 *     outbound records into the send_udp callback, so OpenSSL
 *     never touches sockets directly.
 *   - After the handshake completes, 60 bytes of keying
 *     material are exported and split into client/server SRTP
 *     master keys and salts, then two libsrtp2 sessions are
 *     created (one per direction).
 *
 * The peer certificate fingerprint is verified against the
 * value received in signaling (SDP a=fingerprint line).
 */
#ifndef WEBRTC_DTLS_SRTP_H
#define WEBRTC_DTLS_SRTP_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    DTLS_SRTP_INIT = 0,
    DTLS_SRTP_HANDSHAKING,
    DTLS_SRTP_CONNECTED,
    DTLS_SRTP_FAILED,
    DTLS_SRTP_CLOSED
} DtlsSrtpState;

typedef struct DtlsSrtp DtlsSrtp;

typedef struct {
    void *user;

    /* Send one DTLS or SRTP datagram to the peer. */
    void (*send_udp)(void *user, const uint8_t *packet, size_t len);

    /* Handshake finished and SRTP keys are ready. */
    void (*on_connected)(void *user);

    /* Unprotected RTCP packet received. */
    void (*on_rtcp)(void *user, uint8_t *packet, size_t len);

    /* Called on every state change. */
    void (*on_state)(void *user, DtlsSrtpState new_state);
} DtlsSrtpCallbacks;

/*
 * One time global init: certificate generation, shared
 * SSL_CTX, srtp_init(). Returns 0 on success.
 */
int dtls_srtp_global_init(void);

void dtls_srtp_global_shutdown(void);

/*
 * SHA-256 fingerprint of the local certificate, formatted for
 * SDP as "sha-256 AA:BB:CC:..." (RFC 7999, uppercase, colon
 * separated).
 */
const char *dtls_srtp_local_fingerprint(void);

/*
 * Expect this remote fingerprint (from the SDP offer, value of
 * the a=fingerprint line). The handshake fails when the peer
 * certificate does not match. The hash algorithm token
 * ("sha-256 ") is accepted and ignored; the digest may be
 * colon separated or not, upper or lower case.
 */
void dtls_srtp_set_expected_fingerprint(DtlsSrtp *session,
                                        const char *fingerprint);

DtlsSrtp *dtls_srtp_session_create(const DtlsSrtpCallbacks *callbacks);

void dtls_srtp_session_destroy(DtlsSrtp *session);

/* Feed one DTLS datagram. Drives the handshake. */
void dtls_srtp_on_udp(DtlsSrtp *session,
                      const uint8_t *packet,
                      size_t len);

/* Serve DTLS retransmission timers. Call every loop. */
void dtls_srtp_tick(DtlsSrtp *session);

/* Milliseconds until the next DTLS timer fires, -1 if none. */
int dtls_srtp_next_timeout_ms(const DtlsSrtp *session);

/*
 * Protect and send one RTP packet. pkt points at a complete
 * RTP packet with a 12 byte header; *len is updated in place
 * with the SRTP length. Returns 0 on success.
 */
int dtls_srtp_send_rtp(DtlsSrtp *session, uint8_t *pkt, size_t *len);

/* Same for RTCP packets (SR, SDES, BYE). */
/*
 * Protect and send an RTCP packet. The buffer must hold the RTCP
 * payload plus 14 bytes of headroom (4 for the 64 bit SRTCP header
 * expansion and 10 for the auth tag): protection is in place.
 */
int dtls_srtp_send_rtcp(DtlsSrtp *session, uint8_t *pkt, size_t *len);

/* Unprotect one inbound RTCP datagram. */
int dtls_srtp_unprotect_rtcp(DtlsSrtp *session, uint8_t *pkt, size_t *len);

/* Send a DTLS close_notify and enter DTLS_SRTP_CLOSED. */
void dtls_srtp_close(DtlsSrtp *session);

#endif
