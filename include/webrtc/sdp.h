/*
 * sdp.h
 *
 * Minimal SDP handling for the WebRTC sendonly video path.
 *
 * The browser POSTs an SDP offer to /rtc/offer. We extract:
 *   - ICE ufrag and pwd (used to validate STUN checks)
 *   - DTLS fingerprint (verified against the peer certificate)
 *   - the H264 payload type to send with
 *   - media section mids and setup role
 *
 * The answer advertises exactly one active video media section
 * with an ICE-lite host candidate, the DTLS server
 * fingerprint and the RTP feedback we implement (NACK, PLI).
 */
#ifndef WEBRTC_SDP_H
#define WEBRTC_SDP_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char ice_ufrag[80];
    char ice_pwd[128];
    char fingerprint[128];      /* "sha-256 AA:BB:..." */
    char setup[24];             /* actpass / active / passive */
    char video_mid[24];
    char audio_mid[24];
    int has_audio;
    int h264_payload_type;      /* -1 when the offer has no H264 */
    int h264_packetization_mode;
} SdpOffer;

/*
 * Parse an offer. Returns 0 on success, -1 when a mandatory
 * element is missing.
 */
int sdp_parse_offer(const char *sdp, size_t length, SdpOffer *offer);

/*
 * Build the SDP answer. Returns the number of bytes written
 * ( excluding the terminating zero ) or 0 when out_capacity is
 * too small.
 */
size_t sdp_build_answer(const SdpOffer *offer,
                        const char *local_fingerprint,
                        const char *local_ufrag,
                        const char *local_pwd,
                        const char *advertise_ip,
                        uint16_t udp_port,
                        uint32_t ssrc,
                        char *out,
                        size_t out_capacity);

/*
 * Multi-candidate variant: advertise_ip is used for the c= line
 * and as the first host candidate. extra_ips (if any) are added
 * as additional host candidates with increasing foundation ids.
 * Returns bytes written or 0 on overflow.
 */
size_t sdp_build_answer_multi(const SdpOffer *offer,
                              const char *local_fingerprint,
                              const char *local_ufrag,
                              const char *local_pwd,
                              const char *advertise_ip,
                              const char **extra_ips,
                              size_t extra_count,
                              uint16_t udp_port,
                              uint32_t ssrc,
                              char *out,
                              size_t out_capacity);

#endif
