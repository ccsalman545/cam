/*
 * janus_rtp_sender.h
 *
 * RTP/RTCP bridge between the encoded access unit ring and an
 * external Janus WebRTC gateway, which runs as a separate process.
 *
 * Janus owns everything that makes a WebRTC session WebRTC:
 * signaling (HTTP/WebSocket), ICE, DTLS and SRTP. This module
 * only does what camstream already does well:
 *
 *   AuRing -> rtp_h264_packetize() -> sendto(UDP) -> Janus
 *
 * and a little RTCP glue:
 *
 *   - Sender Reports to Janus's RTCP port every 5 s. Besides
 *     being proper sender behavior, the first report makes Janus
 *     learn this sender's address so it can send feedback back.
 *   - A local RTCP receive port for the PLI/FIR feedback that
 *     Janus forwards when a viewer cannot decode. A PLI/FIR
 *     raises the shared force_idr flag, which the encode worker
 *     already understands (same mechanism the native backend
 *     uses for viewer keyframe requests).
 *
 * The sender runs in its own thread and is the single consumer
 * of the AU ring in Janus transport mode.
 */
#ifndef JANUS_JANUS_RTP_SENDER_H
#define JANUS_JANUS_RTP_SENDER_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "au_ring.h"

#define JANUS_RTP_SENDER_DEFAULT_SSRC 0xC4FEF00Du
#define JANUS_RTP_SENDER_DEFAULT_PT 96

typedef struct JanusRtpSender JanusRtpSender;

typedef struct {
    const char *host;            /* Janus gateway address (IPv4 or name) */
    uint16_t rtp_port;           /* Janus video RTP port   (videoport)     */
    uint16_t rtcp_port;          /* Janus video RTCP port  (videortcpport) */
    uint16_t rtcp_listen_port;   /* local port receiving RTCP from Janus  */
    uint32_t ssrc;               /* 0 = default */
    uint32_t payload_type;       /* 0 = default (96) */
    int verbose;
} JanusRtpSenderConfig;

typedef struct {
    uint64_t access_units;       /* AUs popped from the ring */
    uint32_t packets_sent;       /* RTP packets delivered (sendto ok) */
    uint32_t octets_sent;        /* RTP bytes sent (header + payload) */
    uint32_t send_errors;        /* sendto failures (e.g. Janus down) */
    uint32_t sr_sent;            /* RTCP sender reports to Janus */
    uint32_t rtcp_received;      /* RTCP datagrams received from Janus */
    uint32_t pli_received;       /* picture loss indications */
    uint32_t fir_received;       /* full intraframe requests */
    uint64_t last_rtp_send_ms;   /* 0 = nothing sent yet */
} JanusRtpSenderStats;

/*
 * Create the sender. Opens the send socket and binds the local
 * RTCP port. Returns NULL (with a reason on stderr) when the
 * configuration is invalid or a socket cannot be created.
 *
 * ring:        AU ring to consume (the encoder worker is the producer)
 * force_idr:   shared flag raised when Janus asks for a keyframe
 * au_capacity: per-access-unit buffer size, must fit the largest
 *              access unit the encoder can produce
 */
JanusRtpSender *janus_rtp_sender_create(const JanusRtpSenderConfig *config,
                                        AuRing *ring,
                                        atomic_int *force_idr,
                                        size_t au_capacity);

/* Start the sender thread. Returns 0 or -1. */
int janus_rtp_sender_start(JanusRtpSender *sender);

/* Ask the thread to stop (idempotent). */
void janus_rtp_sender_stop(JanusRtpSender *sender);

/* Wait for the thread to exit (safe even if it never started). */
void janus_rtp_sender_join(JanusRtpSender *sender);

void janus_rtp_sender_destroy(JanusRtpSender *sender);

void janus_rtp_sender_get_stats(JanusRtpSender *sender,
                                JanusRtpSenderStats *out);

uint32_t janus_rtp_sender_ssrc(const JanusRtpSender *sender);
uint32_t janus_rtp_sender_payload_type(const JanusRtpSender *sender);

#endif
