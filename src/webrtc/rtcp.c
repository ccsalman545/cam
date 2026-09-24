/*
 * rtcp.c
 *
 * See rtcp.h. All parsing is bounds checked: a report block is only
 * read when the packet is long enough to contain it.
 */
#include "rtcp.h"

#include <string.h>

#define RTCP_PT_SR 200
#define RTCP_PT_RR 201
#define RTCP_PT_BYE 203
#define RTCP_PT_RTPFB 205
#define RTCP_PT_PSFB 206

#define RTCP_FMT_NACK 1
#define RTCP_FMT_PLI 1
#define RTCP_FMT_FIR 4

#define RTCP_HEADER_SIZE 8
#define RTCP_REPORT_BLOCK_SIZE 24
#define RTCP_SENDER_INFO_SIZE 20

static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t) ((uint16_t) p[0] << 8 | p[1]);
}

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
           ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}

static void write_be16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t) (value >> 8);
    p[1] = (uint8_t) value;
}

static void write_be32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t) (value >> 24);
    p[1] = (uint8_t) (value >> 16);
    p[2] = (uint8_t) (value >> 8);
    p[3] = (uint8_t) value;
}

size_t rtcp_build_sender_report(uint8_t *out,
                                uint32_t ssrc,
                                uint64_t ntp_wall_us,
                                uint32_t rtp_ts,
                                uint32_t packet_count,
                                uint32_t octet_count)
{
    /*
     * NTP timestamp: seconds since 1900-01-01 plus a 32 bit fraction.
     * The 2208988800 offset converts the Unix epoch.
     */
    const uint64_t ntp_secs = ntp_wall_us / 1000000ULL + 2208988800ULL;
    const uint32_t ntp_frac =
        (uint32_t) (((ntp_wall_us % 1000000ULL) << 32) / 1000000ULL);

    out[0] = 0x80;               /* V=2, P=0, RC=0 */
    out[1] = RTCP_PT_SR;
    write_be16(out + 2, 6);      /* length in 32 bit words minus one */
    write_be32(out + 4, ssrc);

    write_be32(out + 8, (uint32_t) (ntp_secs & 0xFFFFFFFFUL));
    write_be32(out + 12, ntp_frac);

    write_be32(out + 16, rtp_ts);
    write_be32(out + 20, packet_count);
    write_be32(out + 24, octet_count);

    return RTCP_SR_SIZE;
}

int rtcp_rtt_ms(uint32_t now_ntp_msw,
                uint32_t rr_last_sr,
                uint32_t rr_delay_since_sr)
{
    if (rr_last_sr == 0) {
        /* The peer has not received a sender report yet. */
        return -1;
    }

    /*
     * RFC 3550 A.3: RTT = A - LSR - DLSR, all three in the compact NTP
     * unit (middle 32 bits, 1/65536 s). The subtraction is modulo 2^32
     * by design (the compact format wraps every 18.2 hours), so the
     * result is read as a signed distance.
     *
     * A small negative value is the rounding of the three 1/65536 s
     * quantities on a sub-millisecond LAN path: the true RTT is 0.
     * Anything further negative means LSR/DLSR do not belong to this
     * clock (a corrupt or foreign report), which is reported as
     * "unknown" rather than turned into a number.
     */
    int32_t elapsed = (int32_t) (now_ntp_msw - rr_last_sr - rr_delay_since_sr);

    if (elapsed < 0) {
        return elapsed > -(65536 / 100) ? 0 : -1;   /* within 10 ms */
    }

    return (int) (((int64_t) elapsed * 1000 + 32768) / 65536);
}

void rtcp_parse(const uint8_t *buffer,
                size_t length,
                uint32_t media_ssrc,
                RtcpFeedback *feedback)
{
    memset(feedback, 0, sizeof(*feedback));

    size_t offset = 0;

    while (offset + 4 <= length) {
        const uint8_t version = buffer[offset] >> 6;

        if (version != 2) {
            return;
        }

        const uint8_t count_or_fmt = buffer[offset] & 0x1F;
        const uint8_t packet_type = buffer[offset + 1];
        const size_t words = read_be16(buffer + offset + 2);
        const size_t packet_len = (words + 1) * 4;

        if (offset + packet_len > length || packet_len < 4) {
            return;
        }

        const uint8_t *body = buffer + offset;
        const size_t body_len = packet_len;

        switch (packet_type) {
        case RTCP_PT_SR:
        case RTCP_PT_RR: {
            /*
             * RFC 3550 6.4: report blocks follow the 8 byte header of an
             * RR, and the 8 byte header plus 20 byte sender info of an
             * SR. Block layout: SSRC(0) fraction(4) cumulative lost(5..7)
             * extended highest seq(8) jitter(12) LSR(16) DLSR(20).
             */
            size_t first = RTCP_HEADER_SIZE +
                           (packet_type == RTCP_PT_SR ? RTCP_SENDER_INFO_SIZE : 0);

            for (size_t i = 0; i < count_or_fmt; i++) {
                size_t at = first + i * RTCP_REPORT_BLOCK_SIZE;

                if (at + RTCP_REPORT_BLOCK_SIZE > body_len) {
                    break;
                }

                const uint8_t *block = body + at;
                uint32_t block_ssrc = read_be32(block);

                /* Keep the block about our stream when the caller names it. */
                if (feedback->has_rr && feedback->rr_ssrc == media_ssrc) {
                    break;
                }

                feedback->has_rr = 1;
                feedback->rr_ssrc = block_ssrc;
                feedback->rr_fraction_lost = block[4];
                feedback->rr_highest_seq = read_be32(block + 8);
                feedback->rr_jitter = read_be32(block + 12);
                feedback->rr_last_sr = read_be32(block + 16);
                feedback->rr_delay_since_sr = read_be32(block + 20);
            }
            break;
        }

        case RTCP_PT_BYE:
            feedback->bye = 1;
            break;

        case RTCP_PT_RTPFB: {
            /*
             * Generic NACK (FMT=1). Each FCI entry is a 16 bit packet
             * id plus a bitmask of the following 16 sequence numbers.
             */
            if (count_or_fmt != RTCP_FMT_NACK) {
                break;
            }

            for (size_t i = offset + 12;
                 i + 4 <= offset + body_len &&
                 feedback->nack_seqs < RTCP_MAX_NACK_SEQS;
                 i += 4) {
                uint16_t pid = read_be16(buffer + i);
                uint16_t mask = read_be16(buffer + i + 2);

                if (feedback->nack_seqs < RTCP_MAX_NACK_SEQS) {
                    feedback->nack_seq[feedback->nack_seqs++] = pid;
                }

                for (int bit = 0;
                     bit < 16 && feedback->nack_seqs < RTCP_MAX_NACK_SEQS;
                     bit++) {
                    if (mask & (1u << bit)) {
                        feedback->nack_seq[feedback->nack_seqs++] =
                            (uint16_t) (pid + bit + 1);
                    }
                }
            }
            break;
        }

        case RTCP_PT_PSFB:
            if (count_or_fmt == RTCP_FMT_PLI) {
                feedback->pli++;
            } else if (count_or_fmt == RTCP_FMT_FIR) {
                feedback->fir++;
            }
            break;

        default:
            break;
        }

        offset += packet_len;
    }
}
