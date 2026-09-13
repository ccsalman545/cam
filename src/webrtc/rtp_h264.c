/*
 * rtp_h264.c
 *
 * RFC 6184 packetizer, see rtp_h264.h.
 */
#include "rtp_h264.h"

#include <stdlib.h>
#include <string.h>

#define H264_NALU_FU_A 28

struct RtpH264 {
    uint32_t ssrc;
    uint32_t payload_type;

    uint16_t sequence;
    uint64_t base_pts_us;
    int have_base;

    uint32_t last_timestamp;

    uint32_t packets;
    uint32_t octets;        /* RTP payload bytes, for the sender report */
};

RtpH264 *rtp_h264_create(void)
{
    RtpH264 *p = calloc(1, sizeof(*p));
    return p;
}

void rtp_h264_destroy(RtpH264 *packetizer)
{
    free(packetizer);
}

void rtp_h264_reset(RtpH264 *packetizer, uint32_t ssrc, uint32_t payload_type)
{
    if (packetizer == NULL) {
        return;
    }

    packetizer->ssrc = ssrc;
    packetizer->payload_type = payload_type;
    packetizer->have_base = 0;
    packetizer->packets = 0;
    packetizer->octets = 0;
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

/*
 * Annex-B start code scanner. Returns the offset and length
 * of the next NAL unit, or 0 when the buffer is exhausted.
 */
static int next_nal(const uint8_t *data, size_t length, size_t cursor,
                    size_t *nal_offset, size_t *nal_length)
{
    /*
     * Find a start code at or after cursor.
     */
    size_t start = cursor;
    size_t code_len = 0;

    while (start + 3 <= length) {
        if (data[start] == 0 && data[start + 1] == 0 && data[start + 2] == 1) {
            code_len = 3;
            break;
        }
        if (start + 4 <= length && data[start] == 0 && data[start + 1] == 0 &&
            data[start + 2] == 0 && data[start + 3] == 1) {
            code_len = 4;
            break;
        }
        start++;
    }

    if (code_len == 0) {
        return 0;
    }

    size_t body = start + code_len;

    /*
     * Find the following start code or end of buffer.
     */
    size_t end = body;

    while (end + 3 <= length) {
        if (data[end] == 0 && data[end + 1] == 0 && data[end + 2] == 1) {
            break;
        }
        if (end + 4 <= length && data[end] == 0 && data[end + 1] == 0 &&
            data[end + 2] == 0 && data[end + 3] == 1) {
            break;
        }
        end++;
    }

    if (end + 3 > length) {
        end = length;
    }

    *nal_offset = body;
    *nal_length = end - body;

    return 1;
}

int rtp_h264_packetize(RtpH264 *p,
                       const uint8_t *access_unit,
                       size_t length,
                       uint64_t pts_us,
                       RtpPacketSink sink,
                       void *user)
{
    if (p == NULL || access_unit == NULL || length == 0 || sink == NULL) {
        return -1;
    }

    if (!p->have_base) {
        p->base_pts_us = pts_us;
        p->have_base = 1;
    }

    uint64_t delta_us = pts_us >= p->base_pts_us ? pts_us - p->base_pts_us : 0;
    uint32_t rtp_ts = (uint32_t) ((delta_us * 90000ULL) / 1000000ULL);

    p->last_timestamp = rtp_ts;

    uint8_t packet[RTP_MAX_PACKET];

    /*
     * RTP fixed header.
     */
    packet[0] = 0x80;

    write_be32(packet + 8, p->ssrc);
    write_be32(packet + 4, rtp_ts);

    size_t cursor = 0;
    int emitted = 0;
    int last_packet_of_au;

    size_t nal_offset = 0;
    size_t nal_length = 0;
    size_t next_cursor = 0;

    /*
     * First pass estimate: does another NAL follow? The
     * marker bit must only be set on the very last packet of
     * the access unit, so we need lookahead while walking.
     */
    while (next_nal(access_unit, length, cursor, &nal_offset, &nal_length)) {
        if (nal_length == 0) {
            cursor = nal_offset;
            continue;
        }

        /*
         * Peek ahead: is there one more NAL after this one?
         */
        size_t probe_offset = 0;
        size_t probe_length = 0;
        size_t after = nal_offset + nal_length;
        int has_more = next_nal(access_unit, length, after,
                                &probe_offset, &probe_length) &&
                       probe_length > 0;

        const uint8_t *nal = access_unit + nal_offset;
        const size_t payload_budget = RTP_MAX_PACKET - RTP_HEADER_SIZE;

        if (nal_length <= payload_budget) {
            /*
             * Single NAL unit packet.
             */
            packet[1] = (uint8_t) p->payload_type;
            write_be16(packet + 2, ++p->sequence);

            memcpy(packet + RTP_HEADER_SIZE, nal, nal_length);

            last_packet_of_au = !has_more;
            packet[1] = (uint8_t) ((last_packet_of_au ? 0x80 : 0x00) |
                                   (p->payload_type & 0x7F));

            sink(user, packet, RTP_HEADER_SIZE + nal_length,
                 last_packet_of_au, p->sequence);

            p->packets++;
            p->octets += (uint32_t) nal_length;
            emitted++;

            next_cursor = after;
        } else {
            /*
             * FU-A fragmentation.
             */
            const uint8_t nri_type = nal[0];
            const size_t chunk = payload_budget - 2;
            size_t offset = 1;      /* skip the NAL header byte */

            while (offset < nal_length) {
                size_t take = nal_length - offset;
                int last = 0;

                if (take > chunk) {
                    take = chunk;
                } else {
                    last = 1;
                }

                last_packet_of_au = last && !has_more;

                packet[1] = (uint8_t) ((last_packet_of_au ? 0x80 : 0x00) |
                                       (p->payload_type & 0x7F));
                write_be16(packet + 2, ++p->sequence);

                packet[RTP_HEADER_SIZE] =
                    (uint8_t) ((nri_type & 0x60) | H264_NALU_FU_A);
                packet[RTP_HEADER_SIZE + 1] =
                    (uint8_t) ((offset == 1 ? 0x80 : 0x00) |
                               (last ? 0x40 : 0x00) |
                               (nri_type & 0x1F));

                memcpy(packet + RTP_HEADER_SIZE + 2, nal + offset, take);

                sink(user, packet, RTP_HEADER_SIZE + 2 + take,
                     last_packet_of_au, p->sequence);

                p->packets++;
                p->octets += (uint32_t) (2 + take);
                emitted++;

                offset += take;
            }

            next_cursor = after;
        }

        /*
         * Continue after this NAL. The scanner needs a fresh
         * cursor at the end position of the current NAL.
         */
        cursor = next_cursor;
    }

    return emitted;
}

uint32_t rtp_h264_packet_count(const RtpH264 *packetizer)
{
    return packetizer != NULL ? packetizer->packets : 0;
}

uint32_t rtp_h264_octet_count(const RtpH264 *packetizer)
{
    return packetizer != NULL ? packetizer->octets : 0;
}

uint32_t rtp_h264_ssrc(const RtpH264 *packetizer)
{
    return packetizer != NULL ? packetizer->ssrc : 0;
}

uint32_t rtp_h264_last_timestamp(const RtpH264 *packetizer)
{
    return packetizer != NULL ? packetizer->last_timestamp : 0;
}
