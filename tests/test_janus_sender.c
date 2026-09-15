#define _POSIX_C_SOURCE 200809L

/*
 * test_janus_sender.c
 *
 * End to end test for the Janus RTP sender, run against a fake
 * local "Janus" made of two plain UDP sockets. No camera, no
 * x264 and no Janus gateway required:
 *
 *   - pushes two synthetic Annex-B access units into the ring
 *     (one with three small NALs, one with a NAL large enough
 *     for FU-A fragmentation)
 *   - verifies the RTP packets the fake Janus receives:
 *     version / PT / SSRC, contiguous sequence numbers, marker
 *     bits, single-NAL vs FU-A payloads, FU-A reconstruction,
 *     90 kHz timestamps
 *   - sends an RTCP PLI to the sender's local RTCP port and
 *     verifies the force_idr flag is raised
 *   - verifies RTCP Sender Reports arrive on the fake Janus
 *     RTCP port with the right SSRC and counters
 *
 * Exit code 0 = pass.
 */
#include <arpa/inet.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "au_ring.h"
#include "janus_rtp_sender.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...)                                             \
    do {                                                             \
        checks++;                                                    \
        if (!(cond)) {                                               \
            failures++;                                              \
            fprintf(stderr, "FAIL: " __VA_ARGS__);                   \
            fprintf(stderr, "  (line %d)\n", __LINE__);              \
        } else {                                                     \
            printf("ok: " __VA_ARGS__);                              \
            printf("\n");                                            \
        }                                                            \
    } while (0)

/*
 * Bind a UDP socket to an ephemeral port; returns the fd and
 * writes the bound port.
 */
static int make_udp_socket(uint16_t *port_out)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (fd < 0) {
        return -1;
    }

    int one = 1;

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = 0;

    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    socklen_t len = sizeof(addr);

    if (getsockname(fd, (struct sockaddr *) &addr, &len) < 0) {
        close(fd);
        return -1;
    }

    *port_out = ntohs(addr.sin_port);
    return fd;
}

static ssize_t recv_with_timeout(int fd, uint8_t *buffer,
                                 size_t capacity, int timeout_ms)
{
    struct pollfd pfd;

    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int ready = poll(&pfd, 1, timeout_ms);

    if (ready <= 0) {
        return 0;
    }

    return recvfrom(fd, buffer, capacity, 0, NULL, NULL);
}

/*
 * AU 1: SPS (17B) + PPS (9B) + IDR slice (121B), mixed 4/3 byte
 * start codes -> three single-NAL packets, marker on the last.
 */
static size_t build_au_one(uint8_t *out)
{
    size_t n = 0;

    out[n++] = 0; out[n++] = 0; out[n++] = 0; out[n++] = 1;
    out[n++] = 0x67;                                /* SPS, nri=3 */
    for (int i = 0; i < 16; i++) {
        out[n++] = (uint8_t) (0xA0 + i);
    }

    out[n++] = 0; out[n++] = 0; out[n++] = 1;
    out[n++] = 0x68;                                /* PPS, nri=3 */
    for (int i = 0; i < 8; i++) {
        out[n++] = (uint8_t) (0xB0 + i);
    }

    out[n++] = 0; out[n++] = 0; out[n++] = 0; out[n++] = 1;
    out[n++] = 0x65;                                /* IDR slice */
    for (int i = 0; i < 120; i++) {
        out[n++] = (uint8_t) i;
    }

    return n;
}

/*
 * AU 2: one 4001 byte non-IDR slice NAL -> FU-A (budget 1186
 * payload bytes per FU: 4 fragments).
 */
#define AU_TWO_NAL_SIZE 4001u

static size_t build_au_two(uint8_t *out)
{
    size_t n = 0;

    out[n++] = 0; out[n++] = 0; out[n++] = 0; out[n++] = 1;
    out[n++] = 0x41;                                /* slice, type 1 */
    for (size_t i = 0; i < AU_TWO_NAL_SIZE - 1; i++) {
        out[n++] = (uint8_t) (i * 7u + 3u);
    }

    return n;
}

static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t) ((uint16_t) p[0] << 8 | p[1]);
}

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
           ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}

int main(void)
{
    uint16_t fake_rtp_port = 0;
    uint16_t fake_rtcp_port = 0;
    uint16_t sender_listen_port = 0;

    int fake_rtp = make_udp_socket(&fake_rtp_port);
    int fake_rtcp = make_udp_socket(&fake_rtcp_port);
    int probe = make_udp_socket(&sender_listen_port);

    if (fake_rtp < 0 || fake_rtcp < 0 || probe < 0) {
        fprintf(stderr, "cannot create UDP sockets\n");
        return 1;
    }

    close(probe);

    /*
     * The fake Janus binds the "videoport" / "videortcpport"; the
     * sender will bind its RTCP listen port itself.
     */
    AuRing *ring = au_ring_create(64 * 1024, 8);

    if (ring == NULL) {
        fprintf(stderr, "ring create failed\n");
        return 1;
    }

    atomic_int force_idr = 0;

    JanusRtpSenderConfig config;

    memset(&config, 0, sizeof(config));
    config.host = "127.0.0.1";
    config.rtp_port = fake_rtp_port;
    config.rtcp_port = fake_rtcp_port;
    config.rtcp_listen_port = sender_listen_port;
    config.ssrc = 0xDEAD1234u;
    config.payload_type = 97;

    JanusRtpSender *sender = janus_rtp_sender_create(&config, ring,
                                                     &force_idr,
                                                     64 * 1024);

    if (sender == NULL) {
        fprintf(stderr, "sender create failed\n");
        return 1;
    }

    if (janus_rtp_sender_start(sender) != 0) {
        fprintf(stderr, "sender start failed\n");
        return 1;
    }

    printf("fake janus: rtp :%u, rtcp :%u; sender rtcp listen :%u\n\n",
           fake_rtp_port, fake_rtcp_port, sender_listen_port);

    /*
     * AU 1.
     */
    uint8_t au1[1024];
    size_t au1_size = build_au_one(au1);

    if (au_ring_push(ring, au1, au1_size, 0, 1) != 0) {
        fprintf(stderr, "push au1 failed\n");
        return 1;
    }

    /*
     * AU 2, 33.333 us later (30 fps).
     */
    uint8_t au2[8192];
    size_t au2_size = build_au_two(au2);

    if (au_ring_push(ring, au2, au2_size, 33333, 0) != 0) {
        fprintf(stderr, "push au2 failed\n");
        return 1;
    }

    /*
     * Collect: 3 single-NAL + 4 FU-A = 7 packets.
     */
    uint8_t packet[2048];
    uint8_t captured[8][1400];
    size_t captured_len[8];
    int packet_count = 0;
    uint16_t first_seq = 0;
    int first_seen = 0;
    int got_all = 0;

    for (int waited = 0; waited < 100 && !got_all; waited++) {
        ssize_t n = recv_with_timeout(fake_rtp, packet, sizeof(packet), 50);

        if (n <= 0) {
            continue;
        }

        packet_count++;

        if (n < 12) {
            CHECK(0, "RTP packet too short (%zd bytes)", n);
            continue;
        }

        if (packet_count < 8 && (size_t) n < sizeof(captured[0])) {
            memcpy(captured[packet_count], packet, (size_t) n);
            captured_len[packet_count] = (size_t) n;
        }

        int version = packet[0] >> 6;
        int marker = (packet[1] >> 7) & 1;
        uint8_t pt = packet[1] & 0x7F;
        uint16_t seq = read_be16(packet + 2);
        uint32_t ts = read_be32(packet + 4);
        uint32_t ssrc = read_be32(packet + 8);

        CHECK(version == 2, "packet %d: version == 2", packet_count);
        CHECK(pt == 97, "packet %d: payload type == 97", packet_count);
        CHECK(ssrc == 0xDEAD1234u, "packet %d: ssrc == 0xDEAD1234",
              packet_count);

        if (!first_seen) {
            first_seen = 1;
            first_seq = seq;
        }

        CHECK(seq == (uint16_t) (first_seq + packet_count - 1),
              "packet %d: contiguous sequence (%u)", packet_count, seq);

        /* NAL header: F(1) NRI(2) TYPE(5) -> type is the low 5 bits. */
        uint8_t nalu_type = packet[12] & 0x1F;

        if (packet_count == 1) {
            CHECK(ts == 0, "AU1 first packet: timestamp == 0");
            CHECK(nalu_type == 7, "packet 1: single NAL, SPS (type 7)");
            CHECK(marker == 0, "packet 1: no marker (more NALs follow)");
            CHECK((size_t) n == 12 + 17, "packet 1: SPS size 17 payload");
        } else if (packet_count == 2) {
            CHECK(nalu_type == 8, "packet 2: single NAL, PPS (type 8)");
            CHECK(marker == 0, "packet 2: no marker (IDR follows)");
            CHECK((size_t) n == 12 + 9, "packet 2: PPS size 9 payload");
        } else if (packet_count == 3) {
            CHECK(nalu_type == 5, "packet 3: single NAL, IDR (type 5)");
            CHECK(marker == 1, "packet 3: marker (last of AU1)");
            CHECK((size_t) n == 12 + 121, "packet 3: IDR size 121");
        } else {
            CHECK(nalu_type == 28, "packet %d: FU-A (type 28)",
                  packet_count);

            int s_bit = (packet[13] >> 7) & 1;
            int e_bit = (packet[13] >> 6) & 1;

            if (packet_count == 4) {
                CHECK(ts == 2999,
                      "AU2 first packet: timestamp 2999 (33.333ms @90kHz)");
                CHECK(s_bit == 1 && e_bit == 0,
                      "packet 4: FU-A start, not end");
                CHECK((packet[13] & 0x1F) == 1,
                      "packet 4: FU-A carries original NAL type (1)");
            }

            if (packet_count == 7) {
                CHECK(s_bit == 0 && e_bit == 1,
                      "packet 7: FU-A end, not start");
                CHECK(marker == 1, "packet 7: marker (last of AU2)");
            } else if (packet_count > 4) {
                CHECK(s_bit == 0 && e_bit == 0,
                      "packet %d: FU-A middle", packet_count);
                CHECK(marker == 0, "packet %d: no marker", packet_count);
            }
        }

        if (packet_count == 7) {
            got_all = 1;
        }
    }

    CHECK(packet_count == 7, "received exactly 7 RTP packets");

    /*
     * Rebuild the FU-A NAL from packets 4-7 and compare it with
     * the original: NAL header byte from the first FU (F/NRI from
     * the FU indicator, type from the FU header), then all FU
     * payloads in order.
     */
    if (packet_count == 7) {
        uint8_t original_nal[AU_TWO_NAL_SIZE];
        uint8_t rebuilt[AU_TWO_NAL_SIZE + 64];
        size_t rebuilt_size = 0;

        memcpy(original_nal, au2 + 4, AU_TWO_NAL_SIZE);

        /*
         * captured[] is indexed by 1-based packet number:
         * packets 4-7 are the four FU-A fragments.
         */
        uint8_t fu_indicator = captured[4][12];
        uint8_t fu_header = captured[4][13];

        rebuilt[0] = (uint8_t) ((fu_indicator & 0xE0) | (fu_header & 0x1F));
        rebuilt_size = 1;

        for (int i = 4; i < 8; i++) {
            size_t payload = captured_len[i] - 14;

            if (payload > 0) {
                memcpy(rebuilt + rebuilt_size,
                       captured[i] + 14,
                       payload);
                rebuilt_size += payload;
            }
        }

        CHECK(rebuilt_size == AU_TWO_NAL_SIZE,
              "FU-A reconstruction: size %zu == %u",
              rebuilt_size, (unsigned) AU_TWO_NAL_SIZE);
        CHECK(memcmp(rebuilt, original_nal, AU_TWO_NAL_SIZE) == 0,
              "FU-A reconstruction: bytes match the original NAL");
    }

    /*
     * Wait for the sender-side counters to settle, then stop.
     */
    usleep(100 * 1000);

    /*
     * PLI: 12 bytes, PT=206, FMT=1, SSRC from "Janus".
     */
    uint8_t pli[12];

    memset(pli, 0, sizeof(pli));
    pli[0] = 0x81;               /* V=2, P=0, FMT=1 (PLI) */
    pli[1] = 206;                /* PT=PSFB */
    pli[4] = 0x4A; pli[5] = 0x4E; pli[6] = 0x41; pli[7] = 0x55;

    struct sockaddr_in to_sender;

    memset(&to_sender, 0, sizeof(to_sender));
    to_sender.sin_family = AF_INET;
    to_sender.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    to_sender.sin_port = htons(sender_listen_port);

    /*
     * Use a throwaway socket for the PLI (the sender does not
     * care who sent the feedback).
     */
    int pli_socket = socket(AF_INET, SOCK_DGRAM, 0);

    if (pli_socket >= 0) {
        if (sendto(pli_socket, pli, sizeof(pli), 0,
                   (struct sockaddr *) &to_sender,
                   sizeof(to_sender)) < 0) {
            CHECK(0, "sending PLI to sender");
        }

        int saw_idr = 0;

        for (int i = 0; i < 100 && !saw_idr; i++) {
            if (atomic_load(&force_idr)) {
                saw_idr = 1;
            }
            usleep(10 * 1000);
        }

        CHECK(saw_idr, "PLI raised the force_idr flag");
        close(pli_socket);
    }

    /*
     * Sender report: the very first SR goes out immediately on
     * thread start; by now at least one must be on the RTCP port.
     */
    uint8_t rtcp_packet[1500];

    ssize_t rtcp_n = 0;

    for (int i = 0; i < 100 && rtcp_n <= 0; i++) {
        rtcp_n = recv_with_timeout(fake_rtcp, rtcp_packet,
                                   sizeof(rtcp_packet), 50);
    }

    CHECK(rtcp_n >= 28, "received an RTCP sender report (%zd bytes)",
          rtcp_n);

    if (rtcp_n >= 28) {
        CHECK((rtcp_packet[0] >> 6) == 2, "SR: version 2");
        CHECK(rtcp_packet[1] == 200, "SR: PT 200 (sender report)");
        CHECK(read_be32(rtcp_packet + 4) == 0xDEAD1234u,
              "SR: SSRC matches the stream");
        CHECK(read_be32(rtcp_packet + 20) == 7,
              "SR: packet count 7 after both AUs");
    }

    janus_rtp_sender_stop(sender);
    janus_rtp_sender_join(sender);

    JanusRtpSenderStats stats;
    janus_rtp_sender_get_stats(sender, &stats);

    CHECK(stats.access_units == 2, "stats: 2 access units consumed");
    CHECK(stats.packets_sent == 7, "stats: 7 RTP packets sent");
    CHECK(stats.pli_received >= 1, "stats: PLI counted");
    CHECK(stats.rtcp_received >= 1, "stats: RTCP datagram counted");
    CHECK(stats.sr_sent >= 1, "stats: at least one SR sent");

    janus_rtp_sender_destroy(sender);
    au_ring_destroy(ring);
    close(fake_rtp);
    close(fake_rtcp);

    printf("\n%d checks, %d failures\n", checks, failures);

    return failures == 0 ? 0 : 1;
}
