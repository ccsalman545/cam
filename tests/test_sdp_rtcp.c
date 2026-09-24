/*
 * test_sdp_rtcp.c
 *
 * Offer parsing and answer shape (codec choice, m-line order), RTCP
 * receiver report parsing and the RFC 3550 A.3 round trip time.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "rtcp.h"
#include "sdp.h"

static int failures;

static void check(int condition, const char *what)
{
    printf("%s: %s\n", condition ? "ok" : "FAIL", what);
    if (!condition) {
        failures++;
    }
}

/*
 * Shape of a Chrome offer: the first H264 payload type is Baseline
 * (42001f), a packetization-mode=0 variant comes before the constrained
 * baseline one, and the video section precedes the audio section.
 */
static const char chrome_offer[] =
    "v=0\r\n"
    "o=- 1 2 IN IP4 127.0.0.1\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "a=group:BUNDLE 0 1\r\n"
    "m=video 9 UDP/TLS/RTP/SAVPF 96 102 103 104 106 127\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=ice-ufrag:abcd\r\n"
    "a=ice-pwd:0123456789abcdefghijklmn\r\n"
    "a=fingerprint:sha-256 AA:BB\r\n"
    "a=setup:actpass\r\n"
    "a=mid:0\r\n"
    "a=recvonly\r\n"
    "a=rtpmap:96 VP8/90000\r\n"
    "a=rtpmap:104 H264/90000\r\n"
    "a=fmtp:104 level-asymmetry-allowed=1;packetization-mode=0;"
        "profile-level-id=42e01f\r\n"
    "a=rtpmap:102 H264/90000\r\n"
    "a=fmtp:102 level-asymmetry-allowed=1;packetization-mode=1;"
        "profile-level-id=42001f\r\n"
    "a=rtpmap:103 rtx/90000\r\n"
    "a=fmtp:103 apt=102\r\n"
    "a=rtpmap:106 H264/90000\r\n"
    "a=fmtp:106 level-asymmetry-allowed=1;packetization-mode=1;"
        "profile-level-id=42e01f\r\n"
    "a=rtpmap:127 H264/90000\r\n"
    "a=fmtp:127 level-asymmetry-allowed=1;packetization-mode=1;"
        "profile-level-id=4d001f\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=mid:1\r\n"
    "a=recvonly\r\n"
    "a=rtpmap:111 opus/48000/2\r\n";

static const char baseline_only_offer[] =
    "v=0\r\n"
    "m=video 9 UDP/TLS/RTP/SAVPF 102 104\r\n"
    "a=ice-ufrag:abcd\r\n"
    "a=ice-pwd:0123456789abcdefghijklmn\r\n"
    "a=fingerprint:sha-256 AA:BB\r\n"
    "a=mid:v\r\n"
    "a=rtpmap:104 H264/90000\r\n"
    "a=fmtp:104 packetization-mode=0;profile-level-id=42001f\r\n"
    "a=rtpmap:102 H264/90000\r\n"
    "a=fmtp:102 packetization-mode=1;profile-level-id=42001f\r\n";

static const char mode0_only_offer[] =
    "v=0\r\n"
    "m=video 9 UDP/TLS/RTP/SAVPF 104\r\n"
    "a=ice-ufrag:abcd\r\n"
    "a=ice-pwd:0123456789abcdefghijklmn\r\n"
    "a=fingerprint:sha-256 AA:BB\r\n"
    "a=mid:v\r\n"
    "a=rtpmap:104 H264/90000\r\n"
    "a=fmtp:104 packetization-mode=0;profile-level-id=42e01f\r\n";

static void test_offer_answer(void)
{
    SdpOffer offer;
    char reason[256] = "";

    check(sdp_parse_offer(chrome_offer, strlen(chrome_offer), &offer,
                          reason, sizeof(reason)) == 0,
          "chrome style offer accepted");
    check(offer.h264_payload_type == 106,
          "constrained baseline packetization-mode=1 chosen (PT 106)");
    check(strcmp(offer.h264_profile_level_id, "42e01f") == 0,
          "chosen profile-level-id recorded");
    check(offer.mline_count == 2 && offer.video_mline == 0,
          "both m-lines recorded, video first");

    const char *ips[1] = { "192.168.0.28" };
    SdpAnswerConfig config;

    memset(&config, 0, sizeof(config));
    config.fingerprint = "sha-256 CC:DD";
    config.ice_ufrag = "uf";
    config.ice_pwd = "pw";
    config.candidate_ips[0] = ips[0];
    config.candidate_count = 1;
    config.udp_port = 50000;
    config.ssrc = 1234;

    char answer[4096];
    size_t length = sdp_build_answer(&offer, &config, answer, sizeof(answer));

    check(length > 0, "answer built");

    const char *video = strstr(answer, "m=video 9 UDP/TLS/RTP/SAVPF 106");
    const char *audio = strstr(answer, "m=audio 0 UDP/TLS/RTP/SAVPF 111");

    check(video != NULL, "answer video section uses PT 106");
    check(audio != NULL, "audio section rejected with port 0");
    check(video != NULL && audio != NULL && video < audio,
          "answer keeps the offer's m-line order");
    check(strstr(answer, "a=fmtp:106 level-asymmetry-allowed=1;"
                         "packetization-mode=1;profile-level-id=42e01f") != NULL,
          "fmtp echoes the chosen profile");
    check(audio != NULL && strstr(audio, "a=mid:1") != NULL,
          "rejected section keeps its mid");
    check(video != NULL && strstr(video, "a=candidate:1 1 udp") != NULL &&
          (audio == NULL || strstr(video, "a=candidate:1 1 udp") < audio),
          "candidates belong to the video section");

    check(sdp_parse_offer(baseline_only_offer, strlen(baseline_only_offer),
                          &offer, reason, sizeof(reason)) == 0 &&
          offer.h264_payload_type == 102,
          "baseline packetization-mode=1 accepted when no constrained one");

    reason[0] = 0;
    check(sdp_parse_offer(mode0_only_offer, strlen(mode0_only_offer),
                          &offer, reason, sizeof(reason)) != 0 &&
          strstr(reason, "packetization-mode=1") != NULL,
          "packetization-mode=0 only offer rejected with a reason");
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t) (v >> 24);
    p[1] = (uint8_t) (v >> 16);
    p[2] = (uint8_t) (v >> 8);
    p[3] = (uint8_t) v;
}

static void test_rtcp(void)
{
    /*
     * Compound packet: RR with one block about SSRC 0x11223344, then a
     * PLI. The PLI header right after the block is what the old parser
     * read as DLSR.
     */
    uint8_t packet[64];

    memset(packet, 0, sizeof(packet));
    packet[0] = 0x81;                   /* V=2, RC=1 */
    packet[1] = 201;                    /* RR */
    packet[3] = 7;                      /* 8 words */
    put_be32(packet + 4, 0xAAAAAAAA);   /* reporter */
    put_be32(packet + 8, 0x11223344);   /* block: source SSRC */
    packet[12] = 25;                    /* fraction lost */
    put_be32(packet + 16, 5000);        /* highest seq */
    put_be32(packet + 20, 321);         /* jitter */
    put_be32(packet + 24, 0x12345678);  /* LSR */
    put_be32(packet + 28, 0x00008000);  /* DLSR: 0.5 s */
    packet[32] = 0x81;                  /* PSFB FMT=1 (PLI) */
    packet[33] = 206;
    packet[35] = 2;

    RtcpFeedback feedback;

    rtcp_parse(packet, 44, 0x11223344, &feedback);

    check(feedback.has_rr && feedback.rr_ssrc == 0x11223344,
          "report block about our SSRC found");
    check(feedback.rr_fraction_lost == 25, "fraction lost at offset 4");
    check(feedback.rr_highest_seq == 5000, "highest seq at offset 8");
    check(feedback.rr_jitter == 321, "jitter at offset 12");
    check(feedback.rr_last_sr == 0x12345678, "LSR at offset 16");
    check(feedback.rr_delay_since_sr == 0x8000, "DLSR at offset 20");
    check(feedback.pli == 1, "PLI in the same compound packet");

    /* 20 ms network round trip on top of the 0.5 s receiver hold. */
    uint32_t now = 0x12345678u + 0x8000u + (uint32_t) (65536 * 20 / 1000);
    int rtt = rtcp_rtt_ms(now, feedback.rr_last_sr,
                          feedback.rr_delay_since_sr);

    check(rtt >= 19 && rtt <= 21, "RTT 20 ms from LSR/DLSR");
    check(rtcp_rtt_ms(0x12345678u + 0x8000u - 2, 0x12345678u, 0x8000u) == 0,
          "sub-millisecond rounding below zero is RTT 0");
    check(rtcp_rtt_ms(0x10000000u, 0x12345678u, 0x8000u) == -1,
          "inconsistent LSR yields unknown, not a huge value");
    check(rtcp_rtt_ms(now, 0, 0) == -1, "no SR echoed yet yields unknown");

    /* Across the 18.2 hour wrap of the compact NTP format. */
    check(rtcp_rtt_ms(0x00000010u, 0xFFFFFF00u, 0x00000010u) ==
          (int) ((0x100 * 1000 + 32768) / 65536),
          "RTT across the compact NTP wrap");
}

int main(int argc, char **argv)
{
    (void) argc;
    (void) argv;

    log_init(LOG_LEVEL_ERROR);

    test_offer_answer();
    test_rtcp();

    if (failures != 0) {
        printf("test_sdp_rtcp: %d failure(s)\n", failures);
        return 1;
    }

    printf("test_sdp_rtcp: PASS\n");
    return 0;
}
