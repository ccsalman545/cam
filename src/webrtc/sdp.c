/*
 * sdp.c
 *
 * Line based SDP parsing and answer generation.
 */
#include "sdp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Copy an attribute value (after "a=xxx:") into a fixed buffer.
 */
static void copy_value(const char *line,
                       const char *prefix,
                       char *out,
                       size_t out_size)
{
    size_t prefix_len = strlen(prefix);

    if (strncmp(line, prefix, prefix_len) != 0) {
        return;
    }

    const char *value = line + prefix_len;

    size_t i = 0;

    while (value[i] != 0 && value[i] != '\r' && value[i] != '\n' &&
           i + 1 < out_size) {
        out[i] = value[i];
        i++;
    }

    out[i] = 0;
}

/*
 * Iterate SDP lines. 'media' is the current media type when
 * the line belongs to a media section: "audio", "video" or "".
 */
typedef void (*LineVisitor)(const char *line,
                            const char *media,
                            void *user);

static void sdp_for_each_line(const char *sdp, size_t length,
                              LineVisitor visit, void *user)
{
    size_t offset = 0;
    char media[16] = "";

    while (offset < length) {
        size_t end = offset;

        while (end < length && sdp[end] != '\n') {
            end++;
        }

        size_t line_len = end - offset;

        while (line_len > 0 &&
               (sdp[offset + line_len - 1] == '\r' ||
                sdp[offset + line_len - 1] == '\n')) {
            line_len--;
        }

        char line[512];

        if (line_len >= sizeof(line)) {
            line_len = sizeof(line) - 1;
        }

        memcpy(line, sdp + offset, line_len);
        line[line_len] = 0;

        if (strncmp(line, "m=", 2) == 0) {
            if (strncmp(line + 2, "audio", 5) == 0) {
                snprintf(media, sizeof(media), "audio");
            } else if (strncmp(line + 2, "video", 5) == 0) {
                snprintf(media, sizeof(media), "video");
            } else {
                media[0] = 0;
            }
        }

        visit(line, media, user);

        offset = end + 1;
    }
}

struct ParseContext {
    SdpOffer *offer;
    int have_ufrag;
    int have_pwd;
    int have_fingerprint;
    char rtpmap_video_line[128];
};

static void parse_visit(const char *line, const char *media, void *user)
{
    struct ParseContext *ctx = user;
    SdpOffer *offer = ctx->offer;

    /*
     * ice-ufrag / ice-pwd appear at session level and inside
     * every media section. Media section values win.
     */
    if (strncmp(line, "a=ice-ufrag:", 12) == 0) {
        copy_value(line, "a=ice-ufrag:", offer->ice_ufrag,
                   sizeof(offer->ice_ufrag));
        ctx->have_ufrag = 1;
    } else if (strncmp(line, "a=ice-pwd:", 10) == 0) {
        copy_value(line, "a=ice-pwd:", offer->ice_pwd,
                   sizeof(offer->ice_pwd));
        ctx->have_pwd = 1;
    } else if (strncmp(line, "a=fingerprint:", 14) == 0) {
        copy_value(line, "a=fingerprint:", offer->fingerprint,
                   sizeof(offer->fingerprint));
        ctx->have_fingerprint = 1;
    } else if (strncmp(line, "a=setup:", 8) == 0) {
        copy_value(line, "a=setup:", offer->setup, sizeof(offer->setup));
    } else if (strcmp(media, "video") == 0 &&
               strncmp(line, "a=mid:", 6) == 0) {
        copy_value(line, "a=mid:", offer->video_mid,
                   sizeof(offer->video_mid));
    } else if (strcmp(media, "audio") == 0 &&
               strncmp(line, "a=mid:", 6) == 0) {
        copy_value(line, "a=mid:", offer->audio_mid,
                   sizeof(offer->audio_mid));
        offer->has_audio = 1;
    } else if (strcmp(media, "video") == 0 &&
               strncmp(line, "a=rtpmap:", 9) == 0) {
        /*
         * a=rtpmap:<pt> H264/90000
         */
        if (strstr(line, "H264/90000") != NULL ||
            strstr(line, "H264/90000\r") != NULL) {
            int pt = atoi(line + 9);

            if (offer->h264_payload_type < 0) {
                offer->h264_payload_type = pt;
            }
        }
    } else if (strcmp(media, "video") == 0 &&
               strncmp(line, "a=fmtp:", 7) == 0) {
        int pt = atoi(line + 7);

        if (pt == offer->h264_payload_type &&
            strstr(line, "packetization-mode=0") != NULL) {
            offer->h264_packetization_mode = 0;
        }
    }
}

int sdp_parse_offer(const char *sdp, size_t length, SdpOffer *offer)
{
    if (sdp == NULL || offer == NULL) {
        return -1;
    }

    memset(offer, 0, sizeof(*offer));
    offer->h264_payload_type = -1;
    offer->h264_packetization_mode = 1;

    struct ParseContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.offer = offer;

    sdp_for_each_line(sdp, length, parse_visit, &ctx);

    if (!ctx.have_ufrag || !ctx.have_pwd || !ctx.have_fingerprint) {
        return -1;
    }

    if (offer->h264_payload_type < 0) {
        fprintf(stderr, "sdp: offer contains no H264 payload type\n");
        return -1;
    }

    if (offer->setup[0] == 0) {
        snprintf(offer->setup, sizeof(offer->setup), "%s", "actpass");
    }

    return 0;
}

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
                              size_t out_capacity)
{
    const char *video_mid = offer->video_mid[0] ? offer->video_mid : "video";
    const char *audio_mid = offer->audio_mid[0] ? offer->audio_mid : "audio";

    char buffer[8192];
    size_t offset = 0;

    /*
     * a=ice-lite is a session-level attribute only (RFC 8839
     * §5.4). Putting it only on the video m-line made some
     * browsers treat us as a full ICE agent and wait for
     * checks we never send.
     */
    int written = snprintf(buffer + offset, sizeof(buffer) - offset,
        "v=0\r\n"
        "o=- 1 1 IN IP4 %s\r\n"
        "s=camstream\r\n"
        "t=0 0\r\n"
        "a=ice-lite\r\n"
        "a=ice-options:trickle\r\n"
        "a=group:BUNDLE %s\r\n"
        "a=msid-semantic: WMS camstream\r\n"
        "a=fingerprint:%s\r\n"
        "a=setup:passive\r\n"
        "a=ice-ufrag:%s\r\n"
        "a=ice-pwd:%s\r\n",
        advertise_ip, video_mid, local_fingerprint, local_ufrag, local_pwd);

    if (written < 0) {
        return 0;
    }
    offset += (size_t) written;

    /*
     * Reject unused audio with port 0 (JSEP §5.3.1). Port 9
     * plus a=inactive is an accepted-but-inactive m-line and
     * needs its own ICE transport when it is not in BUNDLE.
     */
    if (offer->has_audio) {
        written = snprintf(buffer + offset, sizeof(buffer) - offset,
            "m=audio 0 UDP/TLS/RTP/SAVPF 0\r\n"
            "c=IN IP4 0.0.0.0\r\n"
            "a=inactive\r\n"
            "a=mid:%s\r\n",
            audio_mid);

        if (written < 0) {
            return 0;
        }
        offset += (size_t) written;
    }

    written = snprintf(buffer + offset, sizeof(buffer) - offset,
        "m=video 9 UDP/TLS/RTP/SAVPF %d\r\n"
        "c=IN IP4 %s\r\n"
        "a=mid:%s\r\n"
        "a=ice-ufrag:%s\r\n"
        "a=ice-pwd:%s\r\n"
        "a=ice-options:trickle\r\n"
        "a=fingerprint:%s\r\n"
        "a=setup:passive\r\n"
        "a=sendonly\r\n"
        "a=rtcp-mux\r\n"
        "a=msid:camstream camstream-video\r\n"
        "a=ssrc:%u cname:camstream\r\n"
        "a=ssrc:%u msid:camstream camstream-video\r\n"
        "a=rtpmap:%d H264/90000\r\n"
        "a=fmtp:%d packetization-mode=1;profile-level-id=42e01f;"
            "level-asymmetry-allowed=1\r\n"
        "a=rtcp-fb:%d nack\r\n"
        "a=rtcp-fb:%d nack pli\r\n"
        "a=rtcp-fb:%d ccm fir\r\n",
        offer->h264_payload_type,
        advertise_ip,
        video_mid,
        local_ufrag,
        local_pwd,
        local_fingerprint,
        ssrc,
        ssrc,
        offer->h264_payload_type,
        offer->h264_payload_type,
        offer->h264_payload_type,
        offer->h264_payload_type,
        offer->h264_payload_type);

    if (written < 0) {
        return 0;
    }
    offset += (size_t) written;

    /*
     * The candidate lines must follow the RFC 8445 grammar exactly:
     *
     *   candidate:<foundation> <component-id> <transport>
     *            <priority> <connection-address> <port>
     *            typ <candidate-type> [generation <n>]
     *
     * Browsers reject the whole answer when any of the integer
     * fields is missing or out of order.
     *
     * We advertise the primary IP as foundation 1 and any extra
     * local interfaces as additional host candidates. This helps
     * multi-homed machines (Ethernet + Wi-Fi + VPN) and makes the
     * failure mode obvious when the page is opened through a proxy
     * that cannot forward UDP: at least one host candidate will be
     * a private LAN IP that the browser can try directly.
     */
    written = snprintf(buffer + offset, sizeof(buffer) - offset,
        "a=candidate:1 1 udp 2130706431 %s %u typ host generation 0\r\n",
        advertise_ip,
        (unsigned) udp_port);

    if (written < 0) {
        return 0;
    }
    offset += (size_t) written;

    for (size_t i = 0; i < extra_count; i++) {
        const char *ip = extra_ips[i];
        if (ip == NULL || ip[0] == 0) {
            continue;
        }
        if (strcmp(ip, advertise_ip) == 0) {
            continue; /* already advertised as foundation 1 */
        }
        /* foundation = i+2, priority slightly lower for extra interfaces */
        uint32_t prio = 2130706431 - (uint32_t)(i + 1) * 10;
        if (prio < 1000) {
            prio = 1000;
        }
        written = snprintf(buffer + offset, sizeof(buffer) - offset,
            "a=candidate:%zu 1 udp %u %s %u typ host generation 0\r\n",
            i + 2, prio, ip, (unsigned) udp_port);
        if (written < 0) {
            return 0;
        }
        offset += (size_t) written;
        if (offset + 256 > sizeof(buffer)) {
            break; /* avoid overflow, still produce valid SDP */
        }
    }

    written = snprintf(buffer + offset, sizeof(buffer) - offset,
        "a=end-of-candidates\r\n");

    if (written < 0) {
        return 0;
    }
    offset += (size_t) written;

    if (offset + 1 > out_capacity) {
        return 0;
    }

    memcpy(out, buffer, offset + 1);

    return offset;
}

size_t sdp_build_answer(const SdpOffer *offer,
                        const char *local_fingerprint,
                        const char *local_ufrag,
                        const char *local_pwd,
                        const char *advertise_ip,
                        uint16_t udp_port,
                        uint32_t ssrc,
                        char *out,
                        size_t out_capacity)
{
    return sdp_build_answer_multi(offer,
                                  local_fingerprint,
                                  local_ufrag,
                                  local_pwd,
                                  advertise_ip,
                                  NULL,
                                  0,
                                  udp_port,
                                  ssrc,
                                  out,
                                  out_capacity);
}
