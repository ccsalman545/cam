#define _POSIX_C_SOURCE 200809L

/*
 * test_libpeer_stream.c
 *
 * End to end test of camstream-libpeer. A second libpeer instance plays
 * the browser: it fetches the offer over HTTP, answers, posts the answer
 * through the server's sanitizer and then receives the stream over real
 * ICE, DTLS and SRTP. Checks:
 *
 *   1. the first NAL unit a viewer gets is an SPS (the session waits for
 *      an IDR access unit), followed by an IDR slice and a steady stream;
 *      the IDR is large enough to need FU-A fragmentation and reassembly
 *   2. /api/status reports the viewer connected with single-slice frames
 *   3. POST /api/session/close ends the session
 *   4. a viewer that vanishes without closing is detected by the STUN
 *      consent checks and reaped
 *   5. SIGTERM shuts the server down cleanly
 *
 * usage: test_libpeer_stream path/to/camstream-libpeer
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <peer.h>

#define BODY_MAX 65536

static int g_port;

static uint64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000u + (uint64_t) ts.tv_nsec / 1000000u;
}

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

    nanosleep(&ts, NULL);
}

static void die(const char *message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
}

/* ------------------------------------------------------------------ */
/* Minimal HTTP/1.1 client                                             */
/* ------------------------------------------------------------------ */

static int http(const char *method, const char *path, const char *body,
                char *out, size_t out_size)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t) g_port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    /* A wedged server must fail the test, not hang it. */
    struct timeval timeout = { .tv_sec = 5, .tv_usec = 0 };

    if (fd >= 0) {
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    }

    if (fd < 0 || connect(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
        if (fd >= 0) {
            close(fd);
        }
        return -1;
    }

    size_t body_length = body != NULL ? strlen(body) : 0;
    char header[256];
    int header_length = snprintf(header, sizeof(header),
                                 "%s %s HTTP/1.1\r\nHost: localhost\r\n"
                                 "Content-Type: application/json\r\n"
                                 "Content-Length: %zu\r\n\r\n",
                                 method, path, body_length);

    if (write(fd, header, (size_t) header_length) != header_length ||
        (body_length > 0 &&
         write(fd, body, body_length) != (ssize_t) body_length)) {
        close(fd);
        return -1;
    }

    static char response[BODY_MAX + 1024];
    size_t length = 0;
    long content_length = -1;
    char *body_start = NULL;

    for (;;) {
        ssize_t n = read(fd, response + length, sizeof(response) - 1 - length);

        if (n <= 0) {
            break;
        }

        length += (size_t) n;
        response[length] = 0;

        if (body_start == NULL && (body_start = strstr(response, "\r\n\r\n"))) {
            body_start += 4;
            const char *cl = strstr(response, "Content-Length:");
            content_length = cl != NULL ? strtol(cl + 15, NULL, 10) : -1;
        }

        if (body_start != NULL && content_length >= 0 &&
            (long) (length - (size_t) (body_start - response)) >= content_length) {
            break;
        }
    }

    close(fd);

    if (body_start == NULL) {
        return -1;
    }

    snprintf(out, out_size, "%s", body_start);
    return (int) strtol(response + 9, NULL, 10);
}

/* Value of a JSON string field, unescaped (enough for SDP text). */
static int json_string(const char *json, const char *field, char *out,
                       size_t out_size)
{
    char key[64];

    snprintf(key, sizeof(key), "\"%s\":\"", field);
    const char *p = strstr(json, key);

    if (p == NULL) {
        return -1;
    }

    size_t n = 0;

    for (p += strlen(key); *p != 0 && *p != '"' && n + 1 < out_size; p++) {
        if (*p == '\\' && p[1] != 0) {
            p++;
            out[n++] = *p == 'r' ? '\r' : *p == 'n' ? '\n' : *p == 't' ? '\t' : *p;
        } else {
            out[n++] = *p;
        }
    }

    out[n] = 0;
    return 0;
}

static void json_escape(const char *in, char *out, size_t out_size)
{
    size_t n = 0;

    for (; *in != 0 && n + 3 < out_size; in++) {
        if (*in == '\r' || *in == '\n' || *in == '"' || *in == '\\') {
            out[n++] = '\\';
            out[n++] = *in == '\r' ? 'r' : *in == '\n' ? 'n' : *in;
        } else {
            out[n++] = *in;
        }
    }

    out[n] = 0;
}

/* ------------------------------------------------------------------ */
/* The "browser"                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    PeerConnectionState state;
    int nal_count;
    int first_type;
    int idr_count;
    int sps_count;
    int slice_count;
    size_t largest_idr;     /* > one RTP payload proves FU-A reassembly */
} Viewer;

static void on_state(PeerConnectionState state, void *user)
{
    ((Viewer *) user)->state = state;
}

static void on_video(uint8_t *data, size_t size, void *user)
{
    Viewer *viewer = user;

    if (size < 5) {
        return;
    }

    int type = data[4] & 0x1F;

    if (viewer->nal_count++ == 0) {
        viewer->first_type = type;
    }

    viewer->idr_count += type == 5;

    if (type == 5 && size > viewer->largest_idr) {
        viewer->largest_idr = size;
    }

    viewer->sps_count += type == 7;
    viewer->slice_count += type == 1 || type == 5;
}

/*
 * Negotiate one viewer and receive until an IDR and 'slices' slices
 * arrived. Returns the session id; *out_pc is the live connection.
 */
static long connect_viewer(Viewer *viewer, PeerConnection **out_pc,
                           int slices)
{
    static char body[BODY_MAX];
    static char offer[BODY_MAX];
    static char escaped[BODY_MAX];
    static char request[BODY_MAX + 64];

    memset(viewer, 0, sizeof(*viewer));
    viewer->first_type = -1;

    if (http("POST", "/api/session", "", body, sizeof(body)) != 200) {
        fprintf(stderr, "%s\n", body);
        die("POST /api/session");
    }

    char *id_field = strstr(body, "\"id\":");
    long id = id_field != NULL ? strtol(id_field + 5, NULL, 10) : -1;

    if (id <= 0 || json_string(body, "sdp", offer, sizeof(offer)) != 0) {
        die("offer response has no id/sdp");
    }

    if (strstr(offer, "a=setup:passive") == NULL ||
        strstr(offer, "H264/90000") == NULL ||
        strstr(offer, "a=candidate:") == NULL) {
        fprintf(stderr, "%s\n", offer);
        die("offer lacks setup:passive, H264 or a candidate");
    }

    PeerConfiguration config;

    memset(&config, 0, sizeof(config));
    config.video_codec = CODEC_H264;
    config.onvideotrack = on_video;
    config.user_data = viewer;

    PeerConnection *pc = peer_connection_create(&config);

    if (pc == NULL) {
        die("peer_connection_create");
    }

    peer_connection_oniceconnectionstatechange(pc, on_state);
    peer_connection_set_remote_description(pc, offer, SDP_TYPE_OFFER);

    const char *answer = peer_connection_create_answer(pc);

    json_escape(answer, escaped, sizeof(escaped));
    snprintf(request, sizeof(request), "{\"id\":%ld,\"sdp\":\"%s\"}", id,
             escaped);

    if (http("POST", "/api/session/answer", request, body, sizeof(body)) != 200) {
        fprintf(stderr, "%s\n", body);
        die("POST /api/session/answer");
    }

    uint64_t deadline = now_ms() + 20000;
    uint64_t connected_at = 0;

    while (now_ms() < deadline &&
           !(viewer->idr_count > 0 && viewer->slice_count >= slices)) {
        peer_connection_loop(pc);

        if (viewer->state == PEER_CONNECTION_CONNECTED && connected_at == 0) {
            connected_at = now_ms();
        }
    }

    if (viewer->state != PEER_CONNECTION_CONNECTED) {
        die("viewer never reached CONNECTED (ICE/DTLS failed)");
    }

    printf("  session %ld: connected, %d NAL units (%d slices, %d IDR up to "
           "%zu bytes, %d SPS), first NAL type %d\n", id, viewer->nal_count,
           viewer->slice_count, viewer->idr_count, viewer->largest_idr,
           viewer->sps_count, viewer->first_type);

    if (viewer->first_type != 7) {
        die("the first NAL unit was not an SPS: the viewer did not start on "
            "an IDR access unit");
    }

    if (viewer->idr_count == 0 || viewer->slice_count < slices) {
        die("too little video received");
    }

    /* libpeer sends NAL units above 1288 bytes as FU-A fragments. */
    if (viewer->largest_idr <= 1400) {
        die("no fragmented (FU-A) IDR was reassembled");
    }

    *out_pc = pc;
    return id;
}

static int session_listed(long id)
{
    static char body[BODY_MAX];
    char key[32];

    if (http("GET", "/api/status", NULL, body, sizeof(body)) != 200) {
        die("GET /api/status");
    }

    snprintf(key, sizeof(key), "{\"id\":%ld,", id);
    return strstr(body, key) != NULL;
}

static int wait_session_gone(long id, int timeout_ms)
{
    uint64_t start = now_ms();

    while (now_ms() - start < (uint64_t) timeout_ms) {
        if (!session_listed(id)) {
            return (int) (now_ms() - start);
        }
        sleep_ms(200);
    }

    return -1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s path/to/camstream-libpeer\n", argv[0]);
        return 2;
    }

    g_port = 18000 + (int) (getpid() % 2000);

    char port[16];

    snprintf(port, sizeof(port), "%d", g_port);

    pid_t server = fork();

    if (server == 0) {
        execl(argv[1], argv[1], "--test", "-W", "1280", "-H", "720", "-F", "30",
              "-b", "2000", "-K", "1", "-p", port, "--mdns", "off", (char *) NULL);
        perror("exec");
        _exit(127);
    }

    static char body[BODY_MAX];
    uint64_t deadline = now_ms() + 10000;

    while (now_ms() < deadline) {
        if (http("GET", "/api/status", NULL, body, sizeof(body)) == 200 &&
            strstr(body, "\"running\":true") != NULL) {
            break;
        }
        sleep_ms(100);
    }

    if (strstr(body, "\"running\":true") == NULL) {
        kill(server, SIGKILL);
        die("server did not come up with a running pipeline");
    }

    if (peer_init() != 0) {
        die("peer_init");
    }

    /* 1 + 2: stream, then status. */
    Viewer viewer;
    PeerConnection *pc = NULL;
    long id = connect_viewer(&viewer, &pc, 60);

    if (http("GET", "/api/status", NULL, body, sizeof(body)) != 200 ||
        strstr(body, "\"connected\":true") == NULL ||
        strstr(body, "\"multi_slice_frames\":0") == NULL ||
        strstr(body, "\"frames_sent\":0,") != NULL) {
        fprintf(stderr, "%s\n", body);
        die("status does not show a connected single-slice viewer");
    }

    printf("  status: viewer connected, frames sent, single-slice pictures\n");

    /* 3: explicit close. */
    char request[64];

    snprintf(request, sizeof(request), "{\"id\":%ld}", id);
    http("POST", "/api/session/close", request, body, sizeof(body));

    int gone_ms = wait_session_gone(id, 5000);

    if (gone_ms < 0) {
        die("closed session still listed after 5 s");
    }

    printf("  close: session %ld reaped after %d ms\n", id, gone_ms);
    peer_connection_destroy(pc);

    /* 4: a viewer that disappears without a close request. */
    id = connect_viewer(&viewer, &pc, 10);
    peer_connection_destroy(pc);

    gone_ms = wait_session_gone(id, 20000);

    if (gone_ms < 0) {
        die("vanished viewer not detected within 20 s (STUN consent "
            "checks not working)");
    }

    printf("  vanished viewer: detected and reaped after %d ms\n", gone_ms);

    /* 5: clean shutdown. */
    kill(server, SIGTERM);

    int status = 0;

    deadline = now_ms() + 10000;

    while (waitpid(server, &status, WNOHANG) == 0) {
        if (now_ms() > deadline) {
            kill(server, SIGKILL);
            die("server did not exit within 10 s of SIGTERM");
        }
        sleep_ms(50);
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        die("server exit status not 0");
    }

    peer_deinit();
    printf("test_libpeer_stream: all checks passed\n");
    return 0;
}
