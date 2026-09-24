/*
 * rtcp.h
 *
 * RTCP for the video sender: build Sender Reports, parse inbound
 * Receiver Reports and feedback (PLI, FIR, Generic NACK, BYE).
 */
#ifndef WEBRTC_RTCP_H
#define WEBRTC_RTCP_H

#include <stddef.h>
#include <stdint.h>

#define RTCP_SR_SIZE 28
#define RTCP_MAX_NACK_SEQS 128

typedef struct {
    int pli;                        /* picture loss indications */
    int fir;                        /* full intra requests */
    int bye;

    size_t nack_seqs;               /* sequence numbers to retransmit */
    uint16_t nack_seq[RTCP_MAX_NACK_SEQS];

    /* First receiver report block, when present. */
    int has_rr;
    uint8_t rr_fraction_lost;       /* times 256 */
    uint32_t rr_highest_seq;
    uint32_t rr_jitter;             /* RTP clock units */
    uint32_t rr_last_sr;            /* middle 32 bits of the NTP time (LSR) */
    uint32_t rr_delay_since_sr;     /* (DLSR), units of 1/65536 s */
} RtcpFeedback;

/*
 * Build one Sender Report.
 *
 * ntp_wall_us: wall clock (CLOCK_REALTIME) in microseconds.
 * rtp_ts: RTP timestamp corresponding to ntp_wall_us.
 * Returns the number of bytes written.
 */
size_t rtcp_build_sender_report(uint8_t *out,
                                uint32_t ssrc,
                                uint64_t ntp_wall_us,
                                uint32_t rtp_ts,
                                uint32_t packet_count,
                                uint32_t octet_count);

/*
 * Parse a compound RTCP packet (SRTCP already removed). Fields not
 * present stay zero. Unknown packet types are skipped, not rejected.
 */
void rtcp_parse(const uint8_t *buffer,
                size_t length,
                RtcpFeedback *feedback);

/*
 * Round trip time from the last sender report, per RFC 3550 A.3:
 * (now - LSR - DLSR) in milliseconds. now is the middle 32 bits of the
 * NTP time. Returns -1 when the peer has not reported on an SR yet.
 */
int rtcp_rtt_ms(uint32_t now_ntp_msw,
                uint32_t rr_last_sr,
                uint32_t rr_delay_since_sr);

#endif
