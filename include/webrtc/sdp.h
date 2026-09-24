/*
 * sdp.h
 *
 * SDP for the sendonly video path.
 *
 * The browser POSTs an offer; we read out of it:
 *   - ICE ufrag and pwd (validate inbound STUN connectivity checks)
 *   - DTLS fingerprint (verified against the peer certificate)
 *   - the H264 payload type to send with
 *   - media mids and the DTLS setup role
 *
 * The answer describes exactly one active video m-line: our ICE-lite
 * host candidates, the local DTLS fingerprint, setup:passive (the
 * browser is the DTLS client, we are the server) and the feedback we
 * implement (NACK, PLI, FIR).
 */
#ifndef WEBRTC_SDP_H
#define WEBRTC_SDP_H

#include <stddef.h>
#include <stdint.h>

#define SDP_MAX_CANDIDATES 8

#define SDP_MAX_MLINES 8
#define SDP_MAX_H264 16

/* One m= section of the offer, in offer order. */
typedef struct {
    char kind[16];              /* audio, video, application, ... */
    char proto[32];             /* UDP/TLS/RTP/SAVPF, UDP/DTLS/SCTP, ... */
    char fmt[32];               /* first format token, for a rejection */
    char mid[24];
} SdpMediaLine;

/* An H264 payload type of the video section with its fmtp. */
typedef struct {
    int payload_type;
    int packetization_mode;     /* 0 when absent (RFC 6184 default) */
    char profile_level_id[8];   /* six hex digits, "" when absent */
} SdpH264Format;

typedef struct {
    char ice_ufrag[80];
    char ice_pwd[128];
    char fingerprint[128];      /* "sha-256 AA:BB:..." */
    char setup[24];             /* actpass, active or passive */
    char video_mid[24];

    /*
     * Every m= line in offer order. JSEP requires the answer to repeat
     * them in the same order, rejecting (port 0) the ones not used.
     * video_mline is the index of the answered video section, -1 when
     * mline_count is 0 (an offer built by hand in tests).
     */
    SdpMediaLine mlines[SDP_MAX_MLINES];
    size_t mline_count;
    int video_mline;

    SdpH264Format h264[SDP_MAX_H264];
    size_t h264_count;

    /*
     * Chosen H264 format: packetization-mode=1 (FU-A is required for
     * frames larger than one packet), constrained baseline preferred,
     * then baseline. -1 when the offer has no usable H264.
     */
    int h264_payload_type;
    char h264_profile_level_id[8];
} SdpOffer;

typedef struct {
    const char *fingerprint;
    const char *ice_ufrag;
    const char *ice_pwd;
    /* Host candidate addresses; the first one is also the c= address. */
    const char *candidate_ips[SDP_MAX_CANDIDATES];
    size_t candidate_count;
    uint16_t udp_port;
    uint32_t ssrc;
} SdpAnswerConfig;

/*
 * Parse an offer. Returns 0 on success, -1 when a mandatory element is
 * missing, invalid, or when the offer asks for a role we cannot take.
 * The reason is logged and copied into 'reason' (which may be NULL),
 * so the HTTP layer can return it to the browser.
 */
int sdp_parse_offer(const char *sdp,
                    size_t length,
                    SdpOffer *offer,
                    char *reason,
                    size_t reason_size);

/*
 * Build the answer. Returns the number of bytes written (excluding the
 * terminating NUL) or 0 when out_capacity is too small or the request
 * is not answerable (no candidates).
 */
size_t sdp_build_answer(const SdpOffer *offer,
                        const SdpAnswerConfig *config,
                        char *out,
                        size_t out_capacity);

#endif
