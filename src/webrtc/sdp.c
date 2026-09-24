/*
 * sdp.c
 *
 * Line based SDP parsing and answer generation.
 *
 * The parser is deliberately small and strict: everything we do not
 * read is ignored, and an offer that lacks ICE credentials, a
 * fingerprint or an H264 codec is rejected with a reason instead of
 * being answered with a session that can never complete.
 */
#include "sdp.h"

#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SDP_LINE_MAX 512

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
 * Iterate SDP lines. 'media' is the current media type when the line
 * belongs to a media section: "audio", "video" or "".
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

        char line[SDP_LINE_MAX];

        if (line_len >= sizeof(line)) {
            /* Longer than any SDP line we care about: ignore it. */
            offset = end + 1;
            continue;
        }

        memcpy(line, sdp + offset, line_len);
        line[line_len] = 0;

        if (strncmp(line, "m=", 2) == 0) {
            if (strncmp(line + 2, "audio", 5) == 0) {
                memcpy(media, "audio", 6);
            } else if (strncmp(line + 2, "video", 5) == 0) {
                memcpy(media, "video", 6);
            } else {
                media[0] = 0;
            }
        }

        visit(line, media, user);

        offset = end + 1;
    }
}

typedef struct {
    SdpOffer *offer;
    int have_ufrag;
    int have_pwd;
    int have_fingerprint;
} ParseContext;

static void parse_visit(const char *line, const char *media, void *user)
{
    ParseContext *ctx = user;
    SdpOffer *offer = ctx->offer;

    /*
     * ice-ufrag and ice-pwd appear at session level and inside every
     * media section. Media section values win because they are
     * written last in an offer.
     */
    if (strncmp(line, "a=ice-ufrag:", 12) == 0) {
        copy_value(line, "a=ice-ufrag:", offer->ice_ufrag,
                   sizeof(offer->ice_ufrag));
        ctx->have_ufrag = 1;
    } else if (strncmp(line, "a=ice-pwd:", 10) == 0) {
        copy_value(line, "a=ice-pwd:", offer->ice_pwd, sizeof(offer->ice_pwd));
        ctx->have_pwd = 1;
    } else if (strncmp(line, "a=fingerprint:", 14) == 0) {
        copy_value(line, "a=fingerprint:", offer->fingerprint,
                   sizeof(offer->fingerprint));
        ctx->have_fingerprint = 1;
    } else if (strncmp(line, "a=setup:", 8) == 0) {
        copy_value(line, "a=setup:", offer->setup, sizeof(offer->setup));
    } else if (strncmp(line, "a=mid:", 6) == 0) {
        if (strcmp(media, "video") == 0) {
            copy_value(line, "a=mid:", offer->video_mid,
                       sizeof(offer->video_mid));
        } else if (strcmp(media, "audio") == 0) {
            copy_value(line, "a=mid:", offer->audio_mid,
                       sizeof(offer->audio_mid));
            offer->has_audio = 1;
        }
    } else if (strcmp(media, "video") == 0 &&
               strncmp(line, "a=rtpmap:", 9) == 0 &&
               strstr(line, "H264/90000") != NULL) {
        /*
         * a=rtpmap:<pt> H264/90000. The first H264 payload type in the
         * offer is the one we answer with.
         */
        int pt = atoi(line + 9);

        if (offer->h264_payload_type < 0 && pt > 0 && pt < 128) {
            offer->h264_payload_type = pt;
        }
    }
}

/*
 * Report a rejected offer. The message is logged always and copied to
 * the caller's buffer when it has one, so the HTTP response can name
 * the missing or unusable element instead of a generic 400.
 */
static int fail(char *reason, size_t reason_size, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

static int fail(char *reason, size_t reason_size, const char *format, ...)
{
    char message[256];

    va_list args;

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    log_error("sdp", "offer rejected: %s", message);

    if (reason != NULL && reason_size > 0) {
        snprintf(reason, reason_size, "%s", message);
    }

    return -1;
}

int sdp_parse_offer(const char *sdp,
                    size_t length,
                    SdpOffer *offer,
                    char *reason,
                    size_t reason_size)
{
    if (sdp == NULL || offer == NULL) {
        return -1;
    }

    memset(offer, 0, sizeof(*offer));

    offer->h264_payload_type = -1;

    ParseContext ctx;

    memset(&ctx, 0, sizeof(ctx));
    ctx.offer = offer;

    sdp_for_each_line(sdp, length, parse_visit, &ctx);

    if (!ctx.have_ufrag || !ctx.have_pwd) {
        return fail(reason, reason_size,
                    "offer has no ICE credentials (a=ice-ufrag/a=ice-pwd)");
    }

    if (!ctx.have_fingerprint) {
        return fail(reason, reason_size,
                    "offer has no DTLS fingerprint (a=fingerprint)");
    }

    if (offer->h264_payload_type < 0) {
        return fail(reason, reason_size,
                    "offer advertises no H264/90000 video codec");
    }

    if (offer->setup[0] == 0) {
        /* RFC 4145: absent means actpass. */
        snprintf(offer->setup, sizeof(offer->setup), "actpass");
    }

    /*
     * We always answer setup:passive, which requires the peer to be
     * the DTLS client. An offer of "passive" means both sides would
     * wait for a ClientHello.
     */
    if (strcmp(offer->setup, "passive") == 0) {
        return fail(reason, reason_size,
                    "offer requests setup:passive; camstream always answers "
                    "setup:passive and needs a DTLS client");
    }

    return 0;
}

/*
 * Append formatted text, returning the new end offset. Never writes
 * past out_size; when the text does not fit, the result is out_size so
 * the caller's single final check rejects the answer instead of
 * emitting a truncated one.
 */
static size_t append(char *out,
                     size_t out_size,
                     size_t offset,
                     const char *format,
                     ...) __attribute__((format(printf, 4, 5)));

static size_t append(char *out,
                     size_t out_size,
                     size_t offset,
                     const char *format,
                     ...)
{
    if (offset >= out_size) {
        return out_size;
    }

    va_list args;

    va_start(args, format);

    int written = vsnprintf(out + offset, out_size - offset, format, args);

    va_end(args);

    if (written < 0) {
        return out_size;
    }

    size_t total = offset + (size_t) written;

    return total < out_size ? total : out_size;
}

size_t sdp_build_answer(const SdpOffer *offer,
                        const SdpAnswerConfig *config,
                        char *out,
                        size_t out_capacity)
{
    if (offer == NULL || config == NULL || out == NULL ||
        out_capacity == 0 || config->candidate_count == 0 ||
        config->candidate_ips[0] == NULL) {
        return 0;
    }

    const char *video_mid = offer->video_mid[0] ? offer->video_mid : "video";
    const char *audio_mid = offer->audio_mid[0] ? offer->audio_mid : "audio";
    const char *address = config->candidate_ips[0];
    const int pt = offer->h264_payload_type;

    size_t offset = 0;
    size_t limit = out_capacity;

    /*
     * a=ice-lite is session level only (RFC 8839 section 5.4). Putting
     * it on the video m-line alone made some browsers treat us as a
     * full ICE agent and wait for checks we never send.
     */
    offset = append(out, limit, offset,
        "v=0\r\n"
        "o=- 1 1 IN IP4 %s\r\n"
        "s=camstream\r\n"
        "t=0 0\r\n"
        "a=ice-lite\r\n"
        "a=group:BUNDLE %s\r\n"
        "a=msid-semantic: WMS camstream\r\n"
        "a=fingerprint:%s\r\n"
        "a=setup:passive\r\n"
        "a=ice-ufrag:%s\r\n"
        "a=ice-pwd:%s\r\n",
        address, video_mid, config->fingerprint,
        config->ice_ufrag, config->ice_pwd);

    /*
     * Reject unused audio with port 0 (JSEP section 5.3.1).
     */
    if (offer->has_audio) {
        offset = append(out, limit, offset,
            "m=audio 0 UDP/TLS/RTP/SAVPF 0\r\n"
            "c=IN IP4 0.0.0.0\r\n"
            "a=inactive\r\n"
            "a=mid:%s\r\n",
            audio_mid);
    }

    offset = append(out, limit, offset,
        "m=video 9 UDP/TLS/RTP/SAVPF %d\r\n"
        "c=IN IP4 %s\r\n"
        "a=mid:%s\r\n"
        "a=ice-ufrag:%s\r\n"
        "a=ice-pwd:%s\r\n"
        "a=fingerprint:%s\r\n"
        "a=setup:passive\r\n"
        "a=sendonly\r\n"
        "a=rtcp-mux\r\n"
        "a=msid:camstream camstream-video\r\n"
        "a=ssrc:%u cname:camstream\r\n"
        "a=ssrc:%u msid:camstream camstream-video\r\n"
        "a=rtpmap:%d H264/90000\r\n"
        /*
         * profile-level-id 42e01f is constrained baseline, which is
         * what both encoder backends produce. Browsers decode using
         * the SPS in the stream, so this is a negotiation hint.
         * packetization-mode=1 is required for FU-A.
         */
        "a=fmtp:%d packetization-mode=1;profile-level-id=42e01f;"
            "level-asymmetry-allowed=1\r\n"
        "a=rtcp-fb:%d nack\r\n"
        "a=rtcp-fb:%d nack pli\r\n"
        "a=rtcp-fb:%d ccm fir\r\n",
        pt, address, video_mid, config->ice_ufrag, config->ice_pwd,
        config->fingerprint, config->ssrc, config->ssrc,
        pt, pt, pt, pt, pt);

    /*
     * Candidate lines must follow the RFC 8445 grammar exactly:
     *
     *   candidate:<foundation> <component> <transport> <priority>
     *            <address> <port> typ <type> [generation <n>]
     *
     * A browser discards the whole answer when any field is missing or
     * out of order. Extra interfaces (Ethernet plus Wi-Fi, or a VPN)
     * are advertised as additional host candidates so a multi-homed
     * host still connects when the browser reaches it on another
     * address; a broken first candidate must not be the only option.
     */
    offset = append(out, limit, offset,
        "a=candidate:1 1 udp 2130706431 %s %u typ host generation 0\r\n",
        address, (unsigned) config->udp_port);

    for (size_t i = 1; i < config->candidate_count; i++) {
        const char *ip = config->candidate_ips[i];

        if (ip == NULL || ip[0] == 0 || strcmp(ip, address) == 0) {
            continue;
        }

        offset = append(out, limit, offset,
            "a=candidate:%zu 1 udp %u %s %u typ host generation 0\r\n",
            i + 1, 2130706431u - (unsigned) i * 10u, ip,
            (unsigned) config->udp_port);
    }

    offset = append(out, limit, offset, "a=end-of-candidates\r\n");

    if (offset >= limit) {
        log_error("sdp", "answer does not fit in %zu bytes", out_capacity);
        out[0] = 0;
        return 0;
    }

    return offset;
}
