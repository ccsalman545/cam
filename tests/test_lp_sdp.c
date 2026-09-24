/*
 * test_lp_sdp.c
 *
 * lp_sdp_sanitize_answer(): a real browser answer must come through in a
 * form libpeer parses, and anything that would overrun libpeer's fixed
 * size parsing buffers must be dropped or refused.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lp_sdp.h"

static int failures = 0;

#define CHECK(cond, ...)                                             \
    do {                                                             \
        if (!(cond)) {                                               \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);     \
            fprintf(stderr, __VA_ARGS__);                            \
            fprintf(stderr, "\n");                                   \
            failures++;                                              \
        }                                                            \
    } while (0)

/* Shape of a Chrome answer to libpeer's offer, LF line endings. */
#define FP_LOWER "a=fingerprint:sha-256 " \
    "4f:9a:0b:12:34:56:78:9a:bc:de:f0:11:22:33:44:55:" \
    "66:77:88:99:aa:bb:cc:dd:ee:ff:00:12:34:56:78:9a"
#define FP_UPPER "a=fingerprint:sha-256 " \
    "4F:9A:0B:12:34:56:78:9A:BC:DE:F0:11:22:33:44:55:" \
    "66:77:88:99:AA:BB:CC:DD:EE:FF:00:12:34:56:78:9A"

#define HEAD                                                         \
    "v=0\n"                                                          \
    "o=- 4611731400430051336 2 IN IP4 127.0.0.1\n"                   \
    "s=-\n"                                                          \
    "t=0 0\n"                                                        \
    "a=group:BUNDLE 1\n"                                             \
    "a=msid-semantic: WMS\n"                                         \
    "m=video 9 UDP/TLS/RTP/SAVPF 96\n"                               \
    "c=IN IP4 0.0.0.0\n"                                             \
    "a=rtcp:9 IN IP4 0.0.0.0\n"
#define CREDS                                                        \
    "a=ice-ufrag:Xk3r\n"                                             \
    "a=ice-pwd:pZ2mN8qL0vB4tY6uW1eR9sA7\n"                           \
    "a=ice-options:trickle\n"
#define TAIL                                                         \
    "a=mid:1\n"                                                      \
    "a=recvonly\n"                                                   \
    "a=rtcp-mux\n"                                                   \
    "a=rtpmap:96 H264/90000\n"                                       \
    "a=rtcp-fb:96 nack pli\n"                                        \
    "a=fmtp:96 level-asymmetry-allowed=1;packetization-mode=1;"      \
    "profile-level-id=42e01f\n"

#define MDNS_CAND "a=candidate:2893047532 1 udp 2113937151 " \
    "5e0d4e0f-9d4c-4c2b-8f3e-1a2b3c4d5e6f.local 58023 typ host generation 0 " \
    "network-cost 999\n"
#define IPV4_CAND "a=candidate:842163049 1 udp 1677729535 192.168.1.20 " \
    "58023 typ srflx raddr 0.0.0.0 rport 0 generation 0\n"
#define IPV6_CAND "a=candidate:1 1 udp 2122262783 fd00::1 50000 typ host\n"
#define TCP_CAND "a=candidate:3 1 tcp 1518280447 192.168.1.20 9 typ host " \
    "tcptype active\n"

static char out[LP_SDP_MAX_INPUT + 1024];

static int run(const char *sdp, LpSdpAnswerInfo *info, char *error,
               size_t error_size)
{
    error[0] = 0;
    return lp_sdp_sanitize_answer(sdp, out, sizeof(out), info, error,
                                  error_size);
}

static void expect_reject(const char *name, const char *sdp,
                          const char *reason_part)
{
    char error[256];
    LpSdpAnswerInfo info;

    CHECK(run(sdp, &info, error, sizeof(error)) == -1, "%s: accepted", name);
    CHECK(strstr(error, reason_part) != NULL, "%s: reason '%s' lacks '%s'",
          name, error, reason_part);
}

static void test_browser_answer(void)
{
    const char *sdp = HEAD CREDS FP_LOWER "\n" "a=setup:active\n" TAIL
                      MDNS_CAND IPV4_CAND IPV6_CAND TCP_CAND;
    char error[256];
    LpSdpAnswerInfo info;

    CHECK(run(sdp, &info, error, sizeof(error)) == 0, "rejected: %s", error);
    CHECK(info.candidates_kept == 2, "kept %d", info.candidates_kept);
    CHECK(info.mdns_candidates == 1, "mdns %d", info.mdns_candidates);
    CHECK(info.candidates_dropped == 2, "dropped %d", info.candidates_dropped);

    CHECK(strstr(out, FP_UPPER "\r\n") != NULL,
          "fingerprint not upper case / CRLF:\n%s", out);
    CHECK(strstr(out, "fd00::1") == NULL, "IPv6 candidate kept");
    CHECK(strstr(out, " tcp ") == NULL, "TCP candidate kept");
    CHECK(strstr(out, ".local 58023 typ host") != NULL, "mDNS candidate lost");

    /* Every line CRLF terminated, no bare LF (libpeer splits on CRLF). */
    for (const char *p = out; *p != 0; p++) {
        if (*p == '\n') {
            CHECK(p > out && p[-1] == '\r', "bare LF at offset %ld",
                  (long) (p - out));
        }
    }
}

static void test_candidate_limits(void)
{
    char sdp[8192];
    size_t n = (size_t) snprintf(sdp, sizeof(sdp), "%s",
                                 HEAD CREDS FP_UPPER "\na=setup:active\n" TAIL);

    for (int i = 0; i < 12; i++) {
        n += (size_t) snprintf(sdp + n, sizeof(sdp) - n,
                               "a=candidate:%d 1 udp 100 10.0.0.%d 5000 typ "
                               "host\n", i, i + 1);
    }

    char error[256];
    LpSdpAnswerInfo info;

    CHECK(run(sdp, &info, error, sizeof(error)) == 0, "rejected: %s", error);
    CHECK(info.candidates_kept == LP_SDP_MAX_CANDIDATES, "kept %d",
          info.candidates_kept);
    CHECK(info.candidates_dropped == 12 - LP_SDP_MAX_CANDIDATES, "dropped %d",
          info.candidates_dropped);
}

/* Fields libpeer reads with unbounded sscanf("%s"). */
static void test_hostile_candidates(void)
{
    static const char *hostile[] = {
        /* foundation > 32 bytes (libpeer: char foundation[33]) */
        "a=candidate:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA 1 udp 1 "
        "10.0.0.1 5000 typ host\n",
        /* address > 45 bytes (libpeer: char addrstring[46]) */
        "a=candidate:1 1 udp 1 aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaa.local 5000 typ host\n",
        /* type > 15 bytes (libpeer: char type[16]) */
        "a=candidate:1 1 udp 1 10.0.0.1 5000 typ hosthosthosthosthost\n",
        /* transport > 32 bytes (libpeer: char transport[33]) */
        "a=candidate:1 1 udpudpudpudpudpudpudpudpudpudpudpudp 1 10.0.0.1 "
        "5000 typ host\n",
        /* too few fields: sscanf would read into the next line */
        "a=candidate:1 1 udp 1 10.0.0.1 5000\n",
        /* RTCP component, port 0, priority overflow, prflx */
        "a=candidate:1 2 udp 1 10.0.0.1 5000 typ host\n",
        "a=candidate:1 1 udp 1 10.0.0.1 0 typ host\n",
        "a=candidate:1 1 udp 99999999999 10.0.0.1 5000 typ host\n",
        "a=candidate:1 1 udp 1 10.0.0.1 5000 typ prflx\n",
        /* not an mDNS name, not an IPv4 address */
        "a=candidate:1 1 udp 1 evil.example.com 5000 typ host\n",
        "a=candidate:1 1 udp 1 10.0.0.1.2 5000 typ host\n",
    };

    for (size_t i = 0; i < sizeof(hostile) / sizeof(hostile[0]); i++) {
        char sdp[4096];
        char error[256];
        LpSdpAnswerInfo info;

        snprintf(sdp, sizeof(sdp), "%s%s%s",
                 HEAD CREDS FP_UPPER "\na=setup:active\n" TAIL, IPV4_CAND,
                 hostile[i]);

        CHECK(run(sdp, &info, error, sizeof(error)) == 0,
              "case %zu rejected: %s", i, error);
        CHECK(info.candidates_kept == 1 && info.candidates_dropped == 1,
              "case %zu: kept %d dropped %d", i, info.candidates_kept,
              info.candidates_dropped);
    }
}

static void test_rejections(void)
{
    char long_line[2048];

    memset(long_line, 0, sizeof(long_line));
    strcpy(long_line, HEAD CREDS FP_UPPER "\na=setup:active\na=x:");
    memset(long_line + strlen(long_line), 'x', 300);
    strcat(long_line, "\n" IPV4_CAND);
    expect_reject("long line", long_line, "bytes long");

    expect_reject("passive", HEAD CREDS FP_UPPER "\na=setup:passive\n" TAIL
                  IPV4_CAND, "a=setup:active");
    expect_reject("no setup", HEAD CREDS FP_UPPER "\n" TAIL IPV4_CAND,
                  "a=setup:active");
    expect_reject("video rejected",
                  "v=0\nm=video 0 UDP/TLS/RTP/SAVPF 96\n" CREDS FP_UPPER
                  "\na=setup:active\n" IPV4_CAND, "rejected the video");
    expect_reject("no candidates", HEAD CREDS FP_UPPER "\na=setup:active\n"
                  TAIL IPV6_CAND, "ICE gathering");
    expect_reject("sha-1", HEAD CREDS
                  "a=fingerprint:sha-1 4F:9A:0B:12:34:56:78:9A:BC:DE:F0:11:"
                  "22:33:44:55:66:77:88:99\na=setup:active\n" TAIL IPV4_CAND,
                  "sha-256");
    expect_reject("short fingerprint", HEAD CREDS
                  "a=fingerprint:sha-256 4F:9A\na=setup:active\n" TAIL
                  IPV4_CAND, "sha-256");
    expect_reject("no fingerprint", HEAD CREDS "a=setup:active\n" TAIL
                  IPV4_CAND, "fingerprint");
    expect_reject("no ufrag", HEAD "a=ice-pwd:pZ2mN8qL0vB4tY6uW1eR9sA7\n"
                  FP_UPPER "\na=setup:active\n" TAIL IPV4_CAND,
                  "ICE credentials");
    expect_reject("bad ufrag", HEAD "a=ice-ufrag:ab cd\n"
                  "a=ice-pwd:pZ2mN8qL0vB4tY6uW1eR9sA7\n" FP_UPPER
                  "\na=setup:active\n" TAIL IPV4_CAND, "ice-ufrag");
    expect_reject("control char", HEAD CREDS FP_UPPER "\na=setup:active\n"
                  "a=x:\x01\n" TAIL IPV4_CAND, "control character");
    expect_reject("not sdp", "hello\n", "v=0");
    expect_reject("empty", "", "empty");

    char *huge = malloc(LP_SDP_MAX_INPUT + 10);
    if (huge != NULL) {
        memset(huge, 'a', LP_SDP_MAX_INPUT + 9);
        huge[LP_SDP_MAX_INPUT + 9] = 0;
        expect_reject("huge", huge, "too large");
        free(huge);
    }

    /* Valid answer, output buffer too small. */
    char small[64];
    char error[256] = "";

    CHECK(lp_sdp_sanitize_answer(HEAD CREDS FP_UPPER "\na=setup:active\n" TAIL
                                 IPV4_CAND, small, sizeof(small), NULL, error,
                                 sizeof(error)) == -1, "small buffer accepted");
    CHECK(strstr(error, "does not fit") != NULL, "small buffer: %s", error);
}

int main(void)
{
    test_browser_answer();
    test_candidate_limits();
    test_hostile_candidates();
    test_rejections();

    if (failures != 0) {
        fprintf(stderr, "test_lp_sdp: %d failure(s)\n", failures);
        return 1;
    }

    printf("test_lp_sdp: all checks passed\n");
    return 0;
}
