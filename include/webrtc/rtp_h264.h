/*
 * rtp_h264.h
 *
 * RTP packetization of H.264 access units per RFC 6184:
 *   - single NAL unit packets for small NALs
 *   - FU-A fragmentation for NALs larger than the MTU budget
 *
 * The packetizer works on Annex-B access units as produced by
 * both encoder backends and emits complete RTP packets through
 * a callback. The caller protects each packet with SRTP and
 * sends it.
 */
#ifndef WEBRTC_RTP_H264_H
#define WEBRTC_RTP_H264_H

#include <stddef.h>
#include <stdint.h>

#define RTP_HEADER_SIZE 12
#define RTP_MAX_PACKET 1200

typedef struct RtpH264 RtpH264;

/*
 * Called once per RTP packet. packet points to a complete
 * plaintext RTP packet valid only during the call.
 */
typedef void (*RtpPacketSink)(void *user,
                              const uint8_t *packet,
                              size_t len,
                              int marker,
                              uint16_t sequence);

RtpH264 *rtp_h264_create(void);

void rtp_h264_destroy(RtpH264 *packetizer);

/* New stream identity. Call once before the first AU. */
void rtp_h264_reset(RtpH264 *packetizer,
                    uint32_t ssrc,
                    uint32_t payload_type);

/*
 * Packetize one access unit. pts_us is the frame capture
 * timestamp in microseconds (CLOCK_MONOTONIC). Returns the
 * number of RTP packets emitted, or -1 on error.
 */
int rtp_h264_packetize(RtpH264 *packetizer,
                       const uint8_t *access_unit,
                       size_t length,
                       uint64_t pts_us,
                       RtpPacketSink sink,
                       void *user);

uint32_t rtp_h264_packet_count(const RtpH264 *packetizer);
uint32_t rtp_h264_octet_count(const RtpH264 *packetizer);

/* Stream identity, for RTCP sender reports. */
uint32_t rtp_h264_ssrc(const RtpH264 *packetizer);

/*
 * RTP timestamp that corresponds to 'clock_us' on the same monotonic
 * clock as the pts values given to rtp_h264_packetize(). Used to pair
 * the NTP and RTP timestamps of a sender report. 0 before the first
 * packetized access unit.
 */
uint32_t rtp_h264_timestamp_at(const RtpH264 *packetizer, uint64_t clock_us);

#endif
