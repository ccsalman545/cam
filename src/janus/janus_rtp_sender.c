#define _POSIX_C_SOURCE 200809L

/*
 * janus_rtp_sender.c
 *
 * See janus_rtp_sender.h.
 *
 * Thread layout:
 *
 *   sender thread
 *     poll(RTCP listen fd, 20 ms)
 *       - RTCP datagram from Janus: parse, PLI/FIR -> force_idr
 *     drain the AU ring:
 *       - pop access unit
 *       - rtp_h264_packetize() with a sendto() sink
 *     every 5 s:
 *       - RTCP Sender Report to Janus (makes Janus learn our
 *         address so it can send PLI/FIR back)
 */
#include "janus_rtp_sender.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "rtcp.h"
#include "rtp_h264.h"

#define SR_INTERVAL_MS 5000
#define POLL_TIMEOUT_MS 20
#define RTCP_BUF_SIZE 1500

static uint64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t) ts.tv_sec * 1000ULL +
           (uint64_t) ts.tv_nsec / 1000000ULL;
}

/*
 * IPv4 literal first, then getaddrinfo for host names.
 */
static int resolve_host(const char *host, struct in_addr *out)
{
    if (inet_pton(AF_INET, host, out) == 1) {
        return 0;
    }

    struct addrinfo hints;
    struct addrinfo *result = NULL;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    if (getaddrinfo(host, NULL, &hints, &result) != 0 || result == NULL) {
        return -1;
    }

    out->s_addr = ((const struct sockaddr_in *) result->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(result);

    return 0;
}

struct JanusRtpSender {
    /* Destinations on the Janus side. */
    struct sockaddr_in janus_rtp;    /* video RTP packets  */
    struct sockaddr_in janus_rtcp;   /* RTCP sender reports */

    /* Stream identity (must match the Janus mountpoint). */
    uint32_t ssrc;
    uint32_t payload_type;

    /* Media path. */
    AuRing *ring;
    atomic_int *force_idr;
    size_t au_capacity;
    uint8_t *au_buffer;
    RtpH264 *packetizer;

    int send_fd;            /* unconnected UDP send socket */
    int rtcp_listen_fd;     /* bound: RTCP feedback from Janus */

    pthread_t thread;
    atomic_int running;
    int started;
    int verbose;
    char host_name[256];
    uint16_t rtcp_listen_port;

    /* Counters: written by the sender thread, read over HTTP
     * after a stop/join or as a benign snapshot while running
     * (same contract as the encoder worker stats). */
    uint64_t access_units;
    uint32_t packets_sent;
    uint32_t octets_sent;
    uint32_t send_errors;
    uint32_t sr_sent;
    uint32_t rtcp_received;
    uint32_t pli_received;
    uint32_t fir_received;
    uint64_t last_rtp_send_ms;
    int send_error_logged;
};

/*
 * rtp_h264 sink: hand one complete RTP packet to the socket.
 *
 * A vanished or not-yet-started Janus sends ICMP port unreachable,
 * which surfaces here as ECONNREFUSED. Log it once, keep sending:
 * packets are simply dropped until the gateway comes back.
 */
static void send_sink(void *user,
                      const uint8_t *packet,
                      size_t len,
                      int marker,
                      uint16_t sequence)
{
    JanusRtpSender *s = user;

    (void) marker;
    (void) sequence;

    ssize_t sent = sendto(s->send_fd,
                          packet, len, 0,
                          (const struct sockaddr *) &s->janus_rtp,
                          sizeof(s->janus_rtp));

    if (sent < 0) {
        s->send_errors++;

        if (!s->send_error_logged) {
            s->send_error_logged = 1;

            if (errno == ECONNREFUSED) {
                fprintf(stderr,
                        "janus sender: %s:%u is not reachable (is Janus "
                        "running with the matching videoport?). "
                        "Sending continues; packets are dropped until "
                        "the gateway accepts them.\n",
                        s->host_name, ntohs(s->janus_rtp.sin_port));
            } else {
                fprintf(stderr,
                        "janus sender: sendto failed: %s\n",
                        strerror(errno));
            }
        }

        return;
    }

    s->send_error_logged = 0;
    s->packets_sent++;
    s->octets_sent += (uint32_t) len;
    s->last_rtp_send_ms = now_ms();
}

/*
 * Sender report: also the mechanism that makes Janus learn this
 * source's address (Janus only sends PLI back after it has seen a
 * packet from the source on its RTCP port).
 */
static void send_sender_report(JanusRtpSender *s)
{
    uint8_t sr[RTCP_SR_SIZE];
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);

    uint64_t wall_us = (uint64_t) ts.tv_sec * 1000000ULL +
                       (uint64_t) ts.tv_nsec / 1000ULL;

    size_t len = rtcp_build_sender_report(
        sr,
        s->ssrc,
        wall_us,
        rtp_h264_last_timestamp(s->packetizer),
        rtp_h264_packet_count(s->packetizer),
        rtp_h264_octet_count(s->packetizer));

    if (sendto(s->send_fd, sr, len, 0,
               (const struct sockaddr *) &s->janus_rtcp,
               sizeof(s->janus_rtcp)) < 0) {
        /*
         * Non fatal: RTCP is feedback only, media keeps flowing.
         */
        return;
    }

    s->sr_sent++;
}

static void handle_rtcp(JanusRtpSender *s)
{
    uint8_t buffer[RTCP_BUF_SIZE];

    for (;;) {
        ssize_t received = recvfrom(s->rtcp_listen_fd,
                                    buffer, sizeof(buffer),
                                    0, NULL, NULL);

        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }

            /*
             * EAGAIN: queue drained (the socket is
             * non-blocking) -> back to poll().
             */
            break;
        }

        s->rtcp_received++;

        RtcpFeedback feedback;
        rtcp_parse(buffer, (size_t) received, &feedback);

        if (feedback.pli > 0 || feedback.fir > 0) {
            s->pli_received += (uint32_t) feedback.pli;
            s->fir_received += (uint32_t) feedback.fir;

            /*
             * A viewer (via Janus) cannot decode: ask the encoder
             * for a keyframe. The encode worker consumes the flag
             * exactly like the native backend does.
             */
            atomic_store(s->force_idr, 1);

            if (s->verbose) {
                printf("janus sender: RTCP feedback (pli=%d fir=%d), "
                       "requesting keyframe\n",
                       feedback.pli, feedback.fir);
            }
        }
    }
}

static void *sender_thread(void *arg)
{
    JanusRtpSender *s = arg;

    printf("janus sender: started (rtp -> %s:%u, rtcp sr -> %s:%u, "
           "rtcp listen :%u, ssrc %08x, pt %u)\n",
           s->host_name,
           ntohs(s->janus_rtp.sin_port),
           s->host_name,
           ntohs(s->janus_rtcp.sin_port),
           s->rtcp_listen_port,
           s->ssrc,
           s->payload_type);

    uint64_t next_sr_ms = 0;     /* send the first SR immediately */

    while (atomic_load(&s->running)) {
        struct pollfd pfd;

        pfd.fd = s->rtcp_listen_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int ready = poll(&pfd, 1, POLL_TIMEOUT_MS);

        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if (ready > 0 && (pfd.revents & POLLIN)) {
            handle_rtcp(s);
        }

        /*
         * Drain the ring: one access unit in, N RTP packets out.
         */
        for (;;) {
            AuMeta meta;

            if (!au_ring_pop(s->ring, s->au_buffer, s->au_capacity, &meta)) {
                break;
            }

            s->access_units++;

            int packets = rtp_h264_packetize(s->packetizer,
                                             s->au_buffer,
                                             meta.size,
                                             meta.pts_us,
                                             send_sink,
                                             s);

            if (packets < 0) {
                fprintf(stderr, "janus sender: packetize failed (%zu bytes)\n",
                        meta.size);
            } else if (s->verbose && meta.is_idr) {
                printf("janus sender: IDR access unit, %d packets\n",
                       packets);
            }
        }

        uint64_t now = now_ms();

        if (now >= next_sr_ms) {
            next_sr_ms = now + SR_INTERVAL_MS;
            send_sender_report(s);
        }
    }

    printf("janus sender: stopped (%llu AUs, %u packets, %u send errors, "
           "%u pli, %u sr)\n",
           (unsigned long long) s->access_units,
           s->packets_sent,
           s->send_errors,
           s->pli_received,
           s->sr_sent);

    return NULL;
}

JanusRtpSender *janus_rtp_sender_create(const JanusRtpSenderConfig *config,
                                        AuRing *ring,
                                        atomic_int *force_idr,
                                        size_t au_capacity)
{
    if (config == NULL || config->host == NULL || ring == NULL ||
        force_idr == NULL || au_capacity == 0 ||
        config->rtp_port == 0 || config->rtcp_port == 0 ||
        config->rtcp_listen_port == 0) {
        fprintf(stderr, "janus sender: invalid configuration\n");
        return NULL;
    }

    JanusRtpSender *s = calloc(1, sizeof(*s));

    if (s == NULL) {
        return NULL;
    }

    /*
     * destroy() treats >= 0 as "open", so a failed create must not
     * close fd 0.
     */
    s->send_fd = -1;
    s->rtcp_listen_fd = -1;

    s->ssrc = config->ssrc ? config->ssrc : JANUS_RTP_SENDER_DEFAULT_SSRC;
    s->payload_type = config->payload_type ?
        config->payload_type : JANUS_RTP_SENDER_DEFAULT_PT;
    s->verbose = config->verbose;
    s->rtcp_listen_port = config->rtcp_listen_port;
    snprintf(s->host_name, sizeof(s->host_name), "%s", config->host);

    if (resolve_host(config->host, &s->janus_rtp.sin_addr) != 0) {
        fprintf(stderr, "janus sender: cannot resolve host '%s'\n",
                config->host);
        goto fail;
    }

    s->janus_rtcp = s->janus_rtp;
    s->janus_rtp.sin_port = htons(config->rtp_port);
    s->janus_rtcp.sin_port = htons(config->rtcp_port);

    s->send_fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (s->send_fd < 0) {
        fprintf(stderr, "janus sender: socket() failed: %s\n",
                strerror(errno));
        goto fail;
    }

    s->rtcp_listen_fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (s->rtcp_listen_fd < 0) {
        fprintf(stderr, "janus sender: socket() failed: %s\n",
                strerror(errno));
        goto fail;
    }

    int one = 1;

    setsockopt(s->rtcp_listen_fd, SOL_SOCKET, SO_REUSEADDR,
               &one, sizeof(one));

    struct sockaddr_in listen_addr;

    memset(&listen_addr, 0, sizeof(listen_addr));
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_addr.s_addr = INADDR_ANY;
    listen_addr.sin_port = htons(config->rtcp_listen_port);

    if (bind(s->rtcp_listen_fd, (struct sockaddr *) &listen_addr,
             sizeof(listen_addr)) < 0) {
        fprintf(stderr,
                "janus sender: cannot bind RTCP port %u: %s "
                "(change it with --janus-rtcp-listen)\n",
                config->rtcp_listen_port, strerror(errno));
        goto fail;
    }

    /*
     * Non-blocking: handle_rtcp() drains the queue after poll()
     * says there is data, and must return to poll() (and check
     * the stop flag) as soon as the queue is empty.
     */
    {
        int flags = fcntl(s->rtcp_listen_fd, F_GETFL, 0);

        if (flags >= 0) {
            fcntl(s->rtcp_listen_fd, F_SETFL, flags | O_NONBLOCK);
        }
    }

    s->ring = ring;
    s->force_idr = force_idr;
    s->au_capacity = au_capacity;

    s->au_buffer = malloc(au_capacity);

    if (s->au_buffer == NULL) {
        fprintf(stderr, "janus sender: out of memory\n");
        goto fail;
    }

    s->packetizer = rtp_h264_create();

    if (s->packetizer == NULL) {
        goto fail;
    }

    rtp_h264_reset(s->packetizer, s->ssrc, s->payload_type);

    atomic_init(&s->running, 1);

    return s;

fail:
    janus_rtp_sender_destroy(s);
    return NULL;
}

int janus_rtp_sender_start(JanusRtpSender *sender)
{
    if (sender == NULL) {
        return -1;
    }

    if (pthread_create(&sender->thread, NULL, sender_thread, sender) != 0) {
        fprintf(stderr, "janus sender: pthread_create failed\n");
        return -1;
    }

    sender->started = 1;
    return 0;
}

void janus_rtp_sender_stop(JanusRtpSender *sender)
{
    if (sender != NULL) {
        atomic_store(&sender->running, 0);
    }
}

void janus_rtp_sender_join(JanusRtpSender *sender)
{
    if (sender != NULL && sender->started) {
        pthread_join(sender->thread, NULL);
        sender->started = 0;
    }
}

void janus_rtp_sender_destroy(JanusRtpSender *sender)
{
    if (sender == NULL) {
        return;
    }

    janus_rtp_sender_join(sender);

    if (sender->send_fd >= 0) {
        close(sender->send_fd);
    }

    if (sender->rtcp_listen_fd >= 0) {
        close(sender->rtcp_listen_fd);
    }

    rtp_h264_destroy(sender->packetizer);
    free(sender->au_buffer);
    free(sender);
}

void janus_rtp_sender_get_stats(JanusRtpSender *sender,
                                JanusRtpSenderStats *out)
{
    if (out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));

    if (sender == NULL) {
        return;
    }

    out->access_units = sender->access_units;
    out->packets_sent = sender->packets_sent;
    out->octets_sent = sender->octets_sent;
    out->send_errors = sender->send_errors;
    out->sr_sent = sender->sr_sent;
    out->rtcp_received = sender->rtcp_received;
    out->pli_received = sender->pli_received;
    out->fir_received = sender->fir_received;
    out->last_rtp_send_ms = sender->last_rtp_send_ms;
}

uint32_t janus_rtp_sender_ssrc(const JanusRtpSender *sender)
{
    return sender != NULL ? sender->ssrc : 0;
}

uint32_t janus_rtp_sender_payload_type(const JanusRtpSender *sender)
{
    return sender != NULL ? sender->payload_type : 0;
}
