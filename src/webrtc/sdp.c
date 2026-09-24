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
 * Iterate SDP lines. 'media' is the index of the m= section the line
 * belongs to, -1 for session level lines.
 */
typedef void (*LineVisitor)(const char *line, int media, void *user);

static void sdp_for_each_line(const char *sdp, size_t length,
                              LineVisitor visit, void *user)
{
    size_t offset = 0;
    int media = -1;

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
            media++;
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
    int too_many_mlines;
} ParseContext;

/* Copy the n-th space separated token of 'text' (0 based). */
static void copy_token(const char *text, int n, char *out, size_t out_size)
{
    out[0] = 0;

    while (*text != 0) {
        while (*text == ' ') {
            text++;
        }

        size_t len = strcspn(text, " ");

        if (n == 0) {
            if (len >= out_size) {
                len = out_size - 1;
            }
            memcpy(out, text, len);
            out[len] = 0;
            return;
        }

        n--;
        text += len;
    }
}

static SdpH264Format *h264_format(SdpOffer *offer, int pt)
{
    for (size_t i = 0; i < offer->h264_count; i++) {
        if (offer->h264[i].payload_type == pt) {
            return &offer->h264[i];
        }
    }

    return NULL;
}

/* a=fmtp:<pt> key=value;key=value */
static void parse_h264_fmtp(SdpH264Format *format, const char *params)
{
    while (*params != 0) {
        while (*params == ' ' || *params == ';') {
            params++;
        }

        size_t len = strcspn(params, ";");

        if (strncmp(params, "packetization-mode=", 19) == 0) {
            format->packetization_mode = atoi(params + 19);
        } else if (strncmp(params, "profile-level-id=", 17) == 0 &&
                   len == 17 + 6) {
            memcpy(format->profile_level_id, params + 17, 6);
            format->profile_level_id[6] = 0;
        }

        params += len;
    }
}

static void parse_visit(const char *line, int media, void *user)
{
    ParseContext *ctx = user;
    SdpOffer *offer = ctx->offer;

    if (strncmp(line, "m=", 2) == 0) {
        if ((size_t) media >= SDP_MAX_MLINES) {
            ctx->too_many_mlines = 1;
            return;
        }

        SdpMediaLine *m = &offer->mlines[media];

        /* m=<media> <port> <proto> <fmt> ... */
        copy_token(line + 2, 0, m->kind, sizeof(m->kind));
        copy_token(line + 2, 2, m->proto, sizeof(m->proto));
        copy_token(line + 2, 3, m->fmt, sizeof(m->fmt));
        offer->mline_count = (size_t) media + 1;

        if (offer->video_mline < 0 && strcmp(m->kind, "video") == 0) {
            offer->video_mline = media;
        }
        return;
    }

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
    } else if (media >= 0 && (size_t) media < SDP_MAX_MLINES) {
        if (strncmp(line, "a=mid:", 6) == 0) {
            copy_value(line, "a=mid:", offer->mlines[media].mid,
                       sizeof(offer->mlines[media].mid));
            if (media == offer->video_mline) {
                copy_value(line, "a=mid:", offer->video_mid,
                           sizeof(offer->video_mid));
            }
        } else if (media == offer->video_mline &&
                   strncmp(line, "a=rtpmap:", 9) == 0 &&
                   strstr(line, " H264/90000") != NULL) {
            int pt = atoi(line + 9);

            if (pt > 0 && pt < 128 && offer->h264_count < SDP_MAX_H264 &&
                h264_format(offer, pt) == NULL) {
                SdpH264Format *format = &offer->h264[offer->h264_count++];

                memset(format, 0, sizeof(*format));
                format->payload_type = pt;
            }
        } else if (media == offer->video_mline &&
                   strncmp(line, "a=fmtp:", 7) == 0) {
            /* rtpmap precedes fmtp in every browser offer. */
            SdpH264Format *format = h264_format(offer, atoi(line + 7));
            const char *params = strchr(line, ' ');

            if (format != NULL && params != NULL) {
                parse_h264_fmtp(format, params + 1);
            }
        }
    }
}

/*
 * Rank an H264 format for a constrained baseline sender. 0 = unusable.
 * profile_idc 0x42 is Baseline; constraint_set1 (0x40 in the second
 * byte) makes it Constrained Baseline, which is what both encoder
 * backends produce and every WebRTC decoder supports. A Baseline
 * receiver decodes a Constrained Baseline stream too.
 */
static int h264_rank(const SdpH264Format *format)
{
    if (format->packetization_mode != 1) {
        return 0;               /* no FU-A: frames would not fit */
    }

    const char *plid = format->profile_level_id;

    if (plid[0] == 0) {
        return 1;               /* no profile given: baseline default */
    }

    unsigned long value = strtoul(plid, NULL, 16);
    unsigned profile_idc = (unsigned) (value >> 16) & 0xFF;
    unsigned iop = (unsigned) (value >> 8) & 0xFF;

    if (profile_idc == 0x42 || (profile_idc == 0x4D && (iop & 0x80))) {
        return (iop & 0x40) ? 3 : 2;
    }

    return 0;                   /* main/high only: not what we send */
}

static void choose_h264(SdpOffer *offer)
{
    int best_rank = 0;

    for (size_t i = 0; i < offer->h264_count; i++) {
        int rank = h264_rank(&offer->h264[i]);

        if (rank > best_rank) {
            best_rank = rank;
            offer->h264_payload_type = offer->h264[i].payload_type;
            snprintf(offer->h264_profile_level_id,
                     sizeof(offer->h264_profile_level_id), "%s",
                     offer->h264[i].profile_level_id);
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
    offer->video_mline = -1;

    ParseContext ctx;

    memset(&ctx, 0, sizeof(ctx));
    ctx.offer = offer;

    sdp_for_each_line(sdp, length, parse_visit, &ctx);

    if (ctx.too_many_mlines) {
        return fail(reason, reason_size,
                    "offer has more than %d media sections", SDP_MAX_MLINES);
    }

    /* Transport first: text that is not an SDP offer fails here. */
    if (!ctx.have_ufrag || !ctx.have_pwd) {
        return fail(reason, reason_size,
                    "offer has no ICE credentials (a=ice-ufrag/a=ice-pwd)");
    }

    if (!ctx.have_fingerprint) {
        return fail(reason, reason_size,
                    "offer has no DTLS fingerprint (a=fingerprint)");
    }

    if (offer->video_mline < 0) {
        return fail(reason, reason_size, "offer has no video media section "
                    "(the page must add a recvonly video transceiver)");
    }

    choose_h264(offer);

    if (offer->h264_payload_type < 0) {
        if (offer->h264_count > 0) {
            return fail(reason, reason_size,
                        "offer has H264 but no packetization-mode=1 "
                        "baseline profile (%zu H264 formats offered)",
                        offer->h264_count);
        }
        return fail(reason, reason_size,
                    "offer advertises no H264/90000 video codec (browser "
                    "without H264 support?)");
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
    const char *address = config->candidate_ips[0];
    const int pt = offer->h264_payload_type;
    const char *plid = offer->h264_profile_level_id[0]
                           ? offer->h264_profile_level_id : "42e01f";

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
     * One answer section per offered m= line, in offer order (JSEP
     * 5.3.1). Everything but the chosen video section is rejected with
     * port 0. A hand built offer without m-lines gets the video only.
     */
    size_t sections = offer->mline_count > 0 ? offer->mline_count : 1;

    for (size_t m = 0; m < sections; m++) {
        if (offer->mline_count > 0 && (int) m != offer->video_mline) {
            const SdpMediaLine *line = &offer->mlines[m];

            offset = append(out, limit, offset,
                "m=%s 0 %s %s\r\n"
                "c=IN IP4 0.0.0.0\r\n"
                "a=inactive\r\n",
                line->kind[0] ? line->kind : "audio",
                line->proto[0] ? line->proto : "UDP/TLS/RTP/SAVPF",
                line->fmt[0] ? line->fmt : "0");

            if (line->mid[0] != 0) {
                offset = append(out, limit, offset, "a=mid:%s\r\n", line->mid);
            }
            continue;
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
             * The profile-level-id of the chosen offer format is echoed
             * so the browser matches the answer to that exact format.
             * Both encoders produce constrained baseline, which a
             * baseline or constrained baseline receiver decodes.
             * packetization-mode=1 is required for FU-A.
             */
            "a=fmtp:%d level-asymmetry-allowed=1;packetization-mode=1;"
                "profile-level-id=%s\r\n"
            "a=rtcp-fb:%d nack\r\n"
            "a=rtcp-fb:%d nack pli\r\n"
            "a=rtcp-fb:%d ccm fir\r\n",
            pt, address, video_mid, config->ice_ufrag, config->ice_pwd,
            config->fingerprint, config->ssrc, config->ssrc,
            pt, pt, plid, pt, pt, pt);

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
    }

    if (offset >= limit) {
        log_error("sdp", "answer does not fit in %zu bytes", out_capacity);
        out[0] = 0;
        return 0;
    }

    return offset;
}
