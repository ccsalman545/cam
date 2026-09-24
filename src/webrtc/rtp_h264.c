/*
 * rtp_h264.c
 *
 * RFC 6184 packetizer: single NAL unit packets plus FU-A
 * fragmentation. See rtp_h264.h.
 */
#include "rtp_h264.h"

#include <stdlib.h>
#include <string.h>

#define H264_NALU_TYPE_MASK 0x1F
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
    return calloc(1, sizeof(RtpH264));
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
    packetizer->sequence = 0;
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
 * Find the next Annex-B start code at or after 'from'. On success sets
 * the offset of the code and its length (3 or 4 bytes) and returns 1.
 */
static int scan_start_code(const uint8_t *data,
                           size_t length,
                           size_t from,
                           size_t *code_offset,
                           size_t *code_length)
{
    for (size_t i = from; i + 3 <= length; i++) {
        if (data[i] != 0 || data[i + 1] != 0) {
            continue;
        }

        if (data[i + 2] == 1) {
            *code_offset = i;
            *code_length = 3;
            return 1;
        }

        if (i + 4 <= length && data[i + 2] == 0 && data[i + 3] == 1) {
            *code_offset = i;
            *code_length = 4;
            return 1;
        }
    }

    return 0;
}

/*
 * Locate the NAL unit starting at or after 'cursor'. Returns 1 and
 * fills offset/length when found, 0 at the end of the access unit.
 */
static int next_nal(const uint8_t *data,
                    size_t length,
                    size_t cursor,
                    size_t *nal_offset,
                    size_t *nal_length)
{
    size_t code_offset = 0;
    size_t code_length = 0;

    if (!scan_start_code(data, length, cursor, &code_offset, &code_length)) {
        return 0;
    }

    size_t body = code_offset + code_length;
    size_t end = body;
    size_t next_code = 0;
    size_t next_code_length = 0;

    if (scan_start_code(data, length, body, &next_code, &next_code_length)) {
        end = next_code;
    } else {
        end = length;
    }

    *nal_offset = body;
    *nal_length = end > body ? end - body : 0;

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

    /*
     * RTP timestamps are a 90 kHz counter. Deriving them from the
     * capture clock keeps every viewer's timeline identical and
     * unaffected by encode time jitter.
     */
    uint64_t delta_us = pts_us >= p->base_pts_us ? pts_us - p->base_pts_us : 0;
    uint32_t rtp_ts = (uint32_t) ((delta_us * 90000ULL) / 1000000ULL);

    p->last_timestamp = rtp_ts;

    uint8_t packet[RTP_MAX_PACKET];
    const size_t payload_budget = RTP_MAX_PACKET - RTP_HEADER_SIZE;

    packet[0] = 0x80;                   /* V=2, P=0, X=0, CC=0 */
    write_be32(packet + 4, rtp_ts);
    write_be32(packet + 8, p->ssrc);

    size_t cursor = 0;
    size_t nal_offset = 0;
    size_t nal_length = 0;
    int emitted = 0;

    while (next_nal(access_unit, length, cursor, &nal_offset, &nal_length)) {
        cursor = nal_offset + nal_length;

        if (nal_length == 0) {
            continue;
        }

        /*
         * The marker bit belongs on the last packet of the access
         * unit only, so look ahead for a further NAL before emitting
         * the tail of this one.
         */
        size_t probe_offset = 0;
        size_t probe_length = 0;
        int has_more = next_nal(access_unit, length, cursor,
                                &probe_offset, &probe_length) &&
                       probe_length > 0;

        const uint8_t *nal = access_unit + nal_offset;

        if (nal_length <= payload_budget) {
            int last = !has_more;

            packet[1] = (uint8_t) ((last ? 0x80 : 0x00) |
                                   (p->payload_type & 0x7F));
            write_be16(packet + 2, ++p->sequence);

            memcpy(packet + RTP_HEADER_SIZE, nal, nal_length);

            sink(user, packet, RTP_HEADER_SIZE + nal_length,
                 last, p->sequence);

            p->packets++;
            p->octets += (uint32_t) nal_length;
            emitted++;
            continue;
        }

        /*
         * FU-A: the NAL header is replaced by the FU indicator and FU
         * header, and the payload is split across packets of at most
         * payload_budget - 2 bytes each.
         */
        const uint8_t nal_header = nal[0];
        const size_t chunk = payload_budget - 2;
        size_t offset = 1;

        while (offset < nal_length) {
            size_t take = nal_length - offset;
            int last_fragment = 0;

            if (take > chunk) {
                take = chunk;
            } else {
                last_fragment = 1;
            }

            int last = last_fragment && !has_more;

            packet[1] = (uint8_t) ((last ? 0x80 : 0x00) |
                                   (p->payload_type & 0x7F));
            write_be16(packet + 2, ++p->sequence);

            packet[RTP_HEADER_SIZE] =
                (uint8_t) ((nal_header & 0x60) | H264_NALU_FU_A);
            packet[RTP_HEADER_SIZE + 1] =
                (uint8_t) ((offset == 1 ? 0x80 : 0x00) |
                           (last_fragment ? 0x40 : 0x00) |
                           (nal_header & H264_NALU_TYPE_MASK));

            memcpy(packet + RTP_HEADER_SIZE + 2, nal + offset, take);

            sink(user, packet, RTP_HEADER_SIZE + 2 + take,
                 last, p->sequence);

            p->packets++;
            p->octets += (uint32_t) (2 + take);
            emitted++;

            offset += take;
        }
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
