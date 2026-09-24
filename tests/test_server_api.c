/*
 * test_server_api.c
 *
 * End to end test of the management interface. Unlike the other tests,
 * which link one module, this one runs the real binary and speaks HTTP
 * to it, because the things worth checking here only exist in the
 * assembled server:
 *
 *   - the process starts, listens and stops cleanly on SIGTERM
 *   - every documented endpoint answers, unknown ones return 404 JSON
 *   - signaling rejects malformed input without dropping the session list
 *   - a camera that cannot be opened leaves the HTTP interface serving
 *     diagnostics (the operator's only recovery path)
 *   - POST /api/webrtc/restart rotates the DTLS certificate
 *   - POST /api/config/reload re-reads the file
 *
 * usage: test_server_api [path-to-camstream]
 *
 * The binary is a black box: nothing here includes a server header. The
 * test binds 127.0.0.1 on high ports so it can run unprivileged and
 * beside a production instance.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define HTTP_PORT_TEST   18991
#define HTTP_PORT_NO_CAM 18992
#define HTTP_PORT_MANY   18993
#define HTTP_PORT_SHIPPED 18994
#define MDNS_PORT_SHIPPED 15356
#define UDP_PORT_TEST    60990
#define UDP_PORT_NO_CAM  60992
#define UDP_PORT_MANY    60994
#define UDP_PORT_SHIPPED 60996

#define MAX_VIEWERS_TEST 8

#define RESPONSE_MAX 65536
#define START_TIMEOUT_MS 8000
#define STOP_TIMEOUT_MS  8000

static int g_failures;

static void check(int condition, const char *name)
{
    if (condition) {
        printf("  ok   %s\n", name);
    } else {
        printf("  FAIL %s\n", name);
        g_failures++;
    }
}

static void sleep_ms(int milliseconds)
{
    struct timespec delay;

    delay.tv_sec = milliseconds / 1000;
    delay.tv_nsec = (long) (milliseconds % 1000) * 1000000L;

    nanosleep(&delay, NULL);
}

/* ------------------------------------------------------------------ */
/* Files                                                               */
/* ------------------------------------------------------------------ */

static int write_file(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");

    if (file == NULL) {
        return -1;
    }

    size_t length = strlen(text);
    int ok = fwrite(text, 1, length, file) == length;

    fclose(file);

    return ok ? 0 : -1;
}

static int append_file(const char *path, const char *text)
{
    FILE *file = fopen(path, "ab");

    if (file == NULL) {
        return -1;
    }

    size_t length = strlen(text);
    int ok = fwrite(text, 1, length, file) == length;

    fclose(file);

    return ok ? 0 : -1;
}

static int read_file(const char *path,
                     char *out,
                     size_t out_size,
                     size_t *out_length)
{
    FILE *file = fopen(path, "rb");

    if (file == NULL) {
        return -1;
    }

    size_t length = fread(out, 1, out_size - 1, file);

    fclose(file);
    out[length] = 0;
    *out_length = length;

    return 0;
}

/* ------------------------------------------------------------------ */
/* Process control                                                     */
/* ------------------------------------------------------------------ */

static int wait_for_port(uint16_t port, int timeout_ms)
{
    int waited = 0;

    while (waited <= timeout_ms) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);

        if (fd >= 0) {
            struct sockaddr_in address;

            memset(&address, 0, sizeof(address));
            address.sin_family = AF_INET;
            address.sin_port = htons(port);
            inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);

            if (connect(fd, (struct sockaddr *) &address,
                        sizeof(address)) == 0) {
                close(fd);
                return 0;
            }

            close(fd);
        }

        sleep_ms(25);
        waited += 25;
    }

    return -1;
}

static pid_t spawn_server(const char *binary,
                          const char *config_path,
                          const char *log_path,
                          uint16_t port)
{
    pid_t pid = fork();

    if (pid < 0) {
        return -1;
    }

    if (pid == 0) {
        int log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

        if (log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
            close(log_fd);
        }

        execl(binary, "camstream", "--config", config_path, (char *) NULL);
        _exit(127);
    }

    if (wait_for_port(port, START_TIMEOUT_MS) != 0) {
        return -1;
    }

    return pid;
}

/*
 * SIGTERM must end the process with status 0: the server closes sessions,
 * stops the capture thread and frees the DTLS context on the way out, so
 * a non-zero status or a hang here means the shutdown path is broken.
 */
static void stop_server(pid_t pid, const char *name)
{
    if (pid <= 0) {
        return;
    }

    kill(pid, SIGTERM);

    int waited = 0;
    int status = 0;

    while (waited <= STOP_TIMEOUT_MS) {
        pid_t result = waitpid(pid, &status, WNOHANG);

        if (result == pid) {
            check(WIFEXITED(status) && WEXITSTATUS(status) == 0, name);
            return;
        }

        sleep_ms(25);
        waited += 25;
    }

    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    check(0, name);
}

/* ------------------------------------------------------------------ */
/* HTTP client                                                         */
/* ------------------------------------------------------------------ */

/*
 * Every client socket gets a short receive timeout. Without it a server
 * that never answers (a regression worth failing on) would hang the test
 * instead of reporting a failure.
 */
#define CLIENT_TIMEOUT_S 3

static int connect_to(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        return -1;
    }

    struct timeval timeout;

    timeout.tv_sec = CLIENT_TIMEOUT_S;
    timeout.tv_usec = 0;

    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_in address;

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);

    if (connect(fd, (struct sockaddr *) &address, sizeof(address)) != 0) {
        close(fd);
        return -1;
    }

    return fd;
}

/*
 * Sends one request and returns the status code, with the body in
 * 'body'. Connection: close keeps the client simple and still exercises
 * the server's close path.
 */
static int http_call(uint16_t port,
                     const char *method,
                     const char *path,
                     const char *payload,
                     char *body,
                     size_t body_size)
{
    int fd = connect_to(port);

    body[0] = 0;

    if (fd < 0) {
        return -1;
    }

    char request[32768];
    int length;

    if (payload != NULL) {
        length = snprintf(request, sizeof(request),
                          "%s %s HTTP/1.1\r\n"
                          "Host: 127.0.0.1:%u\r\n"
                          "Content-Type: application/json\r\n"
                          "Content-Length: %zu\r\n"
                          "Connection: close\r\n\r\n%s",
                          method, path, port, strlen(payload), payload);
    } else {
        length = snprintf(request, sizeof(request),
                          "%s %s HTTP/1.1\r\n"
                          "Host: 127.0.0.1:%u\r\n"
                          "Connection: close\r\n\r\n",
                          method, path, port);
    }

    if (length <= 0 || (size_t) length >= sizeof(request)) {
        close(fd);
        return -1;
    }

    size_t sent = 0;

    while (sent < (size_t) length) {
        ssize_t written = send(fd, request + sent, (size_t) length - sent, 0);

        if (written <= 0) {
            close(fd);
            return -1;
        }

        sent += (size_t) written;
    }

    static char response[RESPONSE_MAX];
    size_t received = 0;

    while (received + 1 < sizeof(response)) {
        ssize_t got = recv(fd, response + received,
                           sizeof(response) - 1 - received, 0);

        if (got <= 0) {
            break;
        }

        received += (size_t) got;
    }

    response[received] = 0;
    close(fd);

    if (received == 0) {
        return -1;
    }

    int status = 0;

    if (sscanf(response, "HTTP/1.%*d %d", &status) != 1) {
        return -1;
    }

    const char *separator = strstr(response, "\r\n\r\n");

    if (separator == NULL) {
        return status;
    }

    const char *content = separator + 4;
    size_t content_length = strlen(content);

    if (content_length >= body_size) {
        content_length = body_size - 1;
    }

    memcpy(body, content, content_length);
    body[content_length] = 0;

    return status;
}

/*
 * Sends bytes verbatim and closes. Used for input that is not a valid
 * request: the server must survive it and keep answering afterwards.
 */
static int http_send_raw(uint16_t port, const char *bytes, size_t length)
{
    int fd = connect_to(port);

    if (fd < 0) {
        return -1;
    }

    size_t sent = 0;

    while (sent < length) {
        ssize_t written = send(fd, bytes + sent, length - sent, 0);

        if (written <= 0) {
            break;
        }

        sent += (size_t) written;
    }

    /* Read whatever comes back (an error reply, or nothing) and close. */
    char scratch[1024];

    recv(fd, scratch, sizeof(scratch), 0);
    close(fd);

    return 0;
}

/* Minimal JSON string escaper for the SDP payloads used here. */
static void json_escape(const char *input, char *out, size_t out_size)
{
    size_t offset = 0;

    for (size_t i = 0; input[i] != 0 && offset + 7 < out_size; i++) {
        unsigned char c = (unsigned char) input[i];

        if (c == '"' || c == '\\') {
            out[offset++] = '\\';
            out[offset++] = (char) c;
        } else if (c == '\r') {
            offset += (size_t) snprintf(out + offset, out_size - offset, "\\r");
        } else if (c == '\n') {
            offset += (size_t) snprintf(out + offset, out_size - offset, "\\n");
        } else if (c < 0x20) {
            offset += (size_t) snprintf(out + offset, out_size - offset,
                                        "\\u%04x", c);
        } else {
            out[offset++] = (char) c;
        }
    }

    out[offset] = 0;
}

static int body_has(const char *body, const char *needle)
{
    return strstr(body, needle) != NULL;
}

/*
 * Extracts a JSON string value. Enough for "fingerprint" and
 * "session_id": the server's replies are flat and machine generated.
 */
static int json_field(const char *body, const char *field,
                      char *out, size_t out_size)
{
    char pattern[64];

    snprintf(pattern, sizeof(pattern), "\"%s\":", field);

    const char *at = strstr(body, pattern);

    if (at == NULL) {
        return -1;
    }

    at += strlen(pattern);

    if (*at == '"') {
        at++;
        size_t i = 0;

        while (at[i] != 0 && at[i] != '"' && i + 1 < out_size) {
            out[i] = at[i];
            i++;
        }

        out[i] = 0;

        return 0;
    }

    size_t i = 0;

    while (at[i] != 0 && at[i] != ',' && at[i] != '}' && i + 1 < out_size) {
        out[i] = at[i];
        i++;
    }

    out[i] = 0;

    return 0;
}

/* Long form of a JSON integer field, for session ids and counters. */
static long json_long_field(const char *body, const char *field)
{
    char text[32];

    if (json_field(body, field, text, sizeof(text)) != 0) {
        return -1;
    }

    return strtol(text, NULL, 10);
}

/* ------------------------------------------------------------------ */
/* Minimal SDP offer, shaped like a browser's                              */
/* ------------------------------------------------------------------ */

static const char *OFFER_SDP =
    "v=0\r\n"
    "o=- 4611731400430051336 2 IN IP4 127.0.0.1\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "a=group:BUNDLE 0\r\n"
    "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=ice-ufrag:clientuf\r\n"
    "a=ice-pwd:clientpwdclientpwdclientp\r\n"
    "a=fingerprint:sha-256 11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:"
    "11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00\r\n"
    "a=setup:actpass\r\n"
    "a=mid:0\r\n"
    "a=sendrecv\r\n"
    "a=rtcp-mux\r\n"
    "a=rtpmap:96 H264/90000\r\n"
    "a=fmtp:96 level-asymmetry-allowed=1;packetization-mode=1;"
    "profile-level-id=42e01f\r\n";

static int build_offer_body(char *out, size_t out_size)
{
    char escaped[4096];

    json_escape(OFFER_SDP, escaped, sizeof(escaped));

    int written = snprintf(out, out_size, "{\"sdp\":\"%s\"}", escaped);

    return (written > 0 && (size_t) written < out_size) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Config files                                                        */
/* ------------------------------------------------------------------ */

static int write_test_config(const char *path, uint16_t http_port,
                             uint16_t udp_port, uint32_t bitrate_kbps)
{
    char text[1024];

    snprintf(text, sizeof(text),
             "# written by tests/test_server_api.c\n"
             "source = test\n"
             "encoder = sw\n"
             "width = 320\n"
             "height = 240\n"
             "fps = 10\n"
             "bitrate_kbps = %u\n"
             "keyframe_seconds = 2\n"
             "listen = 127.0.0.1\n"
             "http_port = %u\n"
             "udp_port = %u\n"
             "mdns = off\n"
             "verbose = 0\n",
             bitrate_kbps, (unsigned) http_port, (unsigned) udp_port);

    return write_file(path, text);
}

static int write_broken_camera_config(const char *path, uint16_t http_port,
                                      uint16_t udp_port)
{
    char text[1024];

    snprintf(text, sizeof(text),
             "# camera that cannot be opened: the HTTP interface must survive\n"
             "source = v4l2\n"
             "device = /dev/camstream-no-such-camera\n"
             "encoder = sw\n"
             "width = 320\n"
             "height = 240\n"
             "fps = 10\n"
             "bitrate_kbps = 1000\n"
             "listen = 127.0.0.1\n"
             "http_port = %u\n"
             "udp_port = %u\n"
             "mdns = off\n",
             (unsigned) http_port, (unsigned) udp_port);

    return write_file(path, text);
}

/* ------------------------------------------------------------------ */
/* Scenarios                                                           */
/* ------------------------------------------------------------------ */

static void test_management_interface(const char *binary)
{
    const char *config_path = "/tmp/camstream_api_test.conf";
    const char *log_path = "/tmp/camstream_api_test.log";

    printf("test_server_api: management interface\n");

    if (write_test_config(config_path, HTTP_PORT_TEST, UDP_PORT_TEST,
                          1500) != 0) {
        check(0, "write config file");
        return;
    }

    pid_t pid = spawn_server(binary, config_path, log_path, HTTP_PORT_TEST);

    check(pid > 0, "server starts and listens");

    if (pid <= 0) {
        return;
    }

    char body[RESPONSE_MAX];

    check(http_call(HTTP_PORT_TEST, "GET", "/", NULL, body,
                    sizeof(body)) == 200 &&
          body_has(body, "camstream"),
          "GET / serves the web page");

    int status = http_call(HTTP_PORT_TEST, "GET", "/api/status", NULL, body,
                           sizeof(body));

    check(status == 200 && body_has(body, "\"state\"") &&
          body_has(body, "\"source\"") && body_has(body, "\"encoder\"") &&
          body_has(body, "\"webrtc\"") && body_has(body, "\"process\""),
          "GET /api/status reports every subsystem");

    check(body_has(body, "\"fingerprint\":\"sha-256"),
          "GET /api/status carries the DTLS fingerprint");

    status = http_call(HTTP_PORT_TEST, "GET", "/api/stats", NULL, body,
                       sizeof(body));

    check(status == 200 && body_has(body, "\"rtp\"") &&
          body_has(body, "\"sessions\""),
          "GET /api/stats reports RTP counters and sessions");

    status = http_call(HTTP_PORT_TEST, "GET", "/api/logs", NULL, body,
                       sizeof(body));

    check(status == 200 && body_has(body, "\"entries\""),
          "GET /api/logs returns log entries");

    status = http_call(HTTP_PORT_TEST, "GET", "/api/logs?limit=5&level=warn",
                       NULL, body, sizeof(body));

    check(status == 200, "GET /api/logs accepts limit and level");

    status = http_call(HTTP_PORT_TEST, "GET", "/api/nope", NULL, body,
                       sizeof(body));

    check(status == 404 && body_has(body, "\"error\""),
          "unknown route returns 404 JSON");

    status = http_call(HTTP_PORT_TEST, "PUT", "/api/status", NULL, body,
                       sizeof(body));

    check(status == 405, "unsupported method returns 405");

    /* Signaling -------------------------------------------------- */

    char offer[8192];

    if (build_offer_body(offer, sizeof(offer)) != 0) {
        check(0, "build offer body");
        return;
    }

    status = http_call(HTTP_PORT_TEST, "POST", "/api/webrtc/offer", offer,
                       body, sizeof(body));

    char session_id[32] = "";
    char fingerprint_before[256] = "";

    check(status == 200 && json_field(body, "session_id", session_id,
                                      sizeof(session_id)) == 0,
          "POST /api/webrtc/offer returns a session id");

    check(status == 200 && body_has(body, "a=ice-lite") &&
          body_has(body, "a=setup:passive") &&
          body_has(body, "a=candidate:"),
          "answer is ICE-lite with host candidates");

    http_call(HTTP_PORT_TEST, "GET", "/api/status", NULL, body, sizeof(body));
    json_field(body, "fingerprint", fingerprint_before,
               sizeof(fingerprint_before));

    status = http_call(HTTP_PORT_TEST, "POST", "/api/webrtc/restart", NULL,
                       body, sizeof(body));

    char fingerprint_after[256] = "";

    json_field(body, "fingerprint", fingerprint_after,
               sizeof(fingerprint_after));

    check(status == 200 && fingerprint_after[0] != 0 &&
          strcmp(fingerprint_before, fingerprint_after) != 0,
          "POST /api/webrtc/restart rotates the DTLS certificate");

    status = http_call(HTTP_PORT_TEST, "POST", "/api/webrtc/close",
                       "{\"session_id\":1}", body, sizeof(body));

    check(status == 404,
          "POST /api/webrtc/close reports an unknown session");

    status = http_call(HTTP_PORT_TEST, "POST", "/api/webrtc/offer",
                       "{\"not_sdp\":true}", body, sizeof(body));

    check(status == 400 && body_has(body, "\"error\""),
          "offer without an sdp field is rejected");

    status = http_call(HTTP_PORT_TEST, "POST", "/api/webrtc/offer",
                       "{\"sdp\":\"this is not sdp\"}", body, sizeof(body));

    check(status == 400 && body_has(body, "ICE credentials"),
          "malformed offer is rejected with the reason");

    status = http_call(HTTP_PORT_TEST, "POST", "/api/webrtc/offer",
                       offer, body, sizeof(body));

    check(status == 200 && json_field(body, "session_id", session_id,
                                      sizeof(session_id)) == 0,
          "server still accepts a valid offer after bad input");

    /* Management ------------------------------------------------- */

    status = http_call(HTTP_PORT_TEST, "POST", "/api/camera/restart", NULL,
                       body, sizeof(body));

    check(status == 200 && body_has(body, "\"restarted\":true"),
          "POST /api/camera/restart rebuilds the pipeline");

    http_call(HTTP_PORT_TEST, "GET", "/api/stats", NULL, body, sizeof(body));

    check(body_has(body, "\"rtp\""), "stats remain valid after restart");

    /*
     * Reload compares the file with the running values, so an unchanged
     * file reports an empty applied list instead of rewriting settings.
     */
    status = http_call(HTTP_PORT_TEST, "POST", "/api/config/reload", NULL,
                       body, sizeof(body));

    check(status == 200 && body_has(body, "\"applied\":[]"),
          "POST /api/config/reload lists nothing when the file is unchanged");

    if (write_test_config(config_path, HTTP_PORT_TEST, UDP_PORT_TEST,
                          3400) != 0) {
        check(0, "rewrite config file");
    } else {
        status = http_call(HTTP_PORT_TEST, "POST", "/api/config/reload", NULL,
                           body, sizeof(body));

        check(status == 200 &&
              body_has(body, "\"applied\":[\"bitrate_kbps\"]"),
              "reload after an edit reports the changed key");

        status = http_call(HTTP_PORT_TEST, "GET", "/api/status", NULL, body,
                           sizeof(body));

        check(status == 200 && body_has(body, "\"bitrate_kbps\":3400"),
              "status reports the reloaded bitrate");
    }

    if (write_test_config(config_path, HTTP_PORT_TEST, UDP_PORT_TEST,
                          1500) != 0) {
        check(0, "restore config file");
    }

    /* A bad line must be refused without touching the running config. */
    if (write_file(config_path, "source = test\nwidth = 99999\n") != 0) {
        check(0, "write invalid config file");
    } else {
        status = http_call(HTTP_PORT_TEST, "POST", "/api/config/reload", NULL,
                           body, sizeof(body));

        check(status == 400 && body_has(body, "\"error\""),
              "reload refuses an out-of-range value");

        status = http_call(HTTP_PORT_TEST, "GET", "/api/status", NULL, body,
                           sizeof(body));

        check(status == 200, "server keeps serving after a bad reload");
    }

    /*
     * Hostile input: none of this is a valid request, so the only
     * requirement is that the server survives, does not execute
     * anything, and still answers diagnostics afterwards.
     */
    char before[RESPONSE_MAX] = "";

    http_call(HTTP_PORT_TEST, "GET", "/api/status", NULL, before,
              sizeof(before));

    char sessions_before[32] = "";

    json_field(before, "sessions_total", sessions_before,
               sizeof(sessions_before));

    http_send_raw(HTTP_PORT_TEST, "GARBAGE\r\n\r\n", 11);
    http_send_raw(HTTP_PORT_TEST, "GET", 3);
    http_send_raw(HTTP_PORT_TEST, "\r\n\r\n", 4);

    static char long_uri[4200];
    size_t long_uri_len = 0;

    memcpy(long_uri, "GET /", 5);
    long_uri_len = 5;

    while (long_uri_len < sizeof(long_uri) - 16) {
        long_uri[long_uri_len++] = 'a';
    }

    memcpy(long_uri + long_uri_len, " HTTP/1.1\r\n\r\n", 14);
    long_uri_len += 14;

    http_send_raw(HTTP_PORT_TEST, long_uri, long_uri_len);

    static char big_body[20000];

    memset(big_body, 'x', sizeof(big_body));

    {
        char header[256];
        int header_len = snprintf(header, sizeof(header),
                                  "POST /api/webrtc/offer HTTP/1.1\r\n"
                                  "Host: 127.0.0.1:%u\r\n"
                                  "Content-Type: application/json\r\n"
                                  "Content-Length: %zu\r\n"
                                  "Connection: close\r\n\r\n",
                                  (unsigned) HTTP_PORT_TEST, sizeof(big_body));

        int fd = connect_to(HTTP_PORT_TEST);

        if (fd >= 0) {
            status = 0;

            if (send(fd, header, (size_t) header_len, 0) == header_len) {
                send(fd, big_body, sizeof(big_body), 0);
            }

            char scratch[2048];
            ssize_t got = recv(fd, scratch, sizeof(scratch) - 1, 0);

            if (got > 0) {
                scratch[got] = 0;
                sscanf(scratch, "HTTP/1.%*d %d", &status);
            }

            close(fd);

            check(status == 413,
                  "oversized signaling body is rejected with 413");
        } else {
            if (fd >= 0) {
                close(fd);
            }
            check(0, "oversized signaling body is rejected with 413");
        }
    }

    status = http_call(HTTP_PORT_TEST, "GET", "/api/status", NULL, body,
                       sizeof(body));

    check(status == 200 && body_has(body, "\"state\""),
          "server keeps serving after hostile input");

    /* 64 connections opened and closed: sockets and sessions are freed. */
    for (int i = 0; i < 64; i++) {
        http_call(HTTP_PORT_TEST, "GET", "/api/logs?limit=1", NULL, body,
                  sizeof(body));
    }

    status = http_call(HTTP_PORT_TEST, "GET", "/api/stats", NULL, body,
                       sizeof(body));

    check(status == 200, "server survives repeated connections");

    status = http_call(HTTP_PORT_TEST, "GET", "/api/status", NULL, body,
                       sizeof(body));

    char sessions_after[32] = "";

    json_field(body, "sessions_total", sessions_after,
               sizeof(sessions_after));

    check(sessions_before[0] != 0 &&
          strcmp(sessions_before, sessions_after) == 0,
          "no session was created by the hostile input");

    stop_server(pid, "SIGTERM ends the server with status 0");
}

/*
 * Concurrent viewers: every session owns a UDP port and a slot, the
 * ninth viewer is refused, and closing sessions frees both so a new
 * viewer can connect without restarting the server.
 */
static void test_concurrent_viewers(const char *binary)
{
    const char *config_path = "/tmp/camstream_api_many.conf";
    const char *log_path = "/tmp/camstream_api_many.log";

    printf("test_server_api: concurrent viewers\n");

    if (write_test_config(config_path, HTTP_PORT_MANY, UDP_PORT_MANY,
                          1500) != 0) {
        check(0, "write config file for the viewer test");
        return;
    }

    pid_t pid = spawn_server(binary, config_path, log_path, HTTP_PORT_MANY);

    check(pid > 0, "server starts for the viewer test");

    if (pid <= 0) {
        return;
    }

    char offer[8192];

    if (build_offer_body(offer, sizeof(offer)) != 0) {
        check(0, "build offer body");
        stop_server(pid, "SIGTERM ends the viewer server");
        return;
    }

    char body[RESPONSE_MAX];
    char session_id[MAX_VIEWERS_TEST + 1][32];
    int accepted = 0;

    for (int i = 0; i < MAX_VIEWERS_TEST; i++) {
        int status = http_call(HTTP_PORT_MANY, "POST", "/api/webrtc/offer",
                               offer, body, sizeof(body));

        if (status != 200 ||
            json_field(body, "session_id", session_id[i],
                       sizeof(session_id[i])) != 0) {
            break;
        }

        accepted++;
    }

    check(accepted == MAX_VIEWERS_TEST,
          "eight concurrent sessions are accepted");

    int status = http_call(HTTP_PORT_MANY, "GET", "/api/status", NULL, body,
                           sizeof(body));

    check(status == 200 && json_long_field(body, "sessions_active") ==
          MAX_VIEWERS_TEST,
          "status counts all eight sessions as active");

    check(body_has(body, "\"session_create_failures\":0"),
          "no session failed to start");

    status = http_call(HTTP_PORT_MANY, "POST", "/api/webrtc/offer", offer,
                       body, sizeof(body));

    check(status == 503 && body_has(body, "session limit"),
          "the ninth viewer is refused with the limit named");

    /* Close two viewers; their slots and UDP ports must come back. */
    char close_body[64];

    for (int i = 0; i < 2 && accepted > i; i++) {
        snprintf(close_body, sizeof(close_body), "{\"session_id\":%.*s}",
                 (int) sizeof(session_id[i]) - 1, session_id[i]);

        status = http_call(HTTP_PORT_MANY, "POST", "/api/webrtc/close",
                           close_body, body, sizeof(body));

        check(status == 200, i == 0 ? "closing a session succeeds"
                                    : "closing a second session succeeds");
    }

    status = http_call(HTTP_PORT_MANY, "GET", "/api/status", NULL, body,
                       sizeof(body));

    check(status == 200 && json_long_field(body, "sessions_active") ==
          MAX_VIEWERS_TEST - 2,
          "closing sessions frees their slots");

    status = http_call(HTTP_PORT_MANY, "POST", "/api/webrtc/offer", offer,
                       body, sizeof(body));

    check(status == 200,
          "a freed slot accepts a new viewer without a restart");

    /* Close everything again and wait for the sockets to be released. */
    for (int i = 2; i < accepted; i++) {
        snprintf(close_body, sizeof(close_body), "{\"session_id\":%.*s}",
                 (int) sizeof(session_id[i]) - 1, session_id[i]);

        http_call(HTTP_PORT_MANY, "POST", "/api/webrtc/close", close_body,
                  body, sizeof(body));
    }

    http_call(HTTP_PORT_MANY, "POST", "/api/webrtc/restart", NULL, body,
              sizeof(body));

    status = http_call(HTTP_PORT_MANY, "GET", "/api/status", NULL, body,
                       sizeof(body));

    check(status == 200 &&
          json_long_field(body, "sessions_active") == 0,
          "restart drops every remaining session");

    status = http_call(HTTP_PORT_MANY, "POST", "/api/webrtc/offer", offer,
                       body, sizeof(body));

    check(status == 200,
          "signaling works again after dropping every session");

    stop_server(pid, "SIGTERM ends the viewer server cleanly");
}

static void test_camera_failure_keeps_http_up(const char *binary)
{
    const char *config_path = "/tmp/camstream_api_nocam.conf";
    const char *log_path = "/tmp/camstream_api_nocam.log";

    printf("test_server_api: camera failure\n");

    if (write_broken_camera_config(config_path, HTTP_PORT_NO_CAM,
                                   UDP_PORT_NO_CAM) != 0) {
        check(0, "write broken camera config");
        return;
    }

    pid_t pid = spawn_server(binary, config_path, log_path,
                             HTTP_PORT_NO_CAM);

    check(pid > 0, "server starts without a working camera");

    if (pid <= 0) {
        return;
    }

    char body[RESPONSE_MAX];
    int status = http_call(HTTP_PORT_NO_CAM, "GET", "/api/status", NULL, body,
                           sizeof(body));

    check(status == 200 && body_has(body, "camera-error"),
          "status reports camera-error while HTTP stays up");

    check(body_has(body, "\"status\":\"failed\""),
          "source is reported as failed");

    status = http_call(HTTP_PORT_NO_CAM, "GET", "/api/logs?level=error", NULL,
                       body, sizeof(body));

    check(status == 200 && body_has(body, "v4l2"),
          "logs name the failing subsystem");

    status = http_call(HTTP_PORT_NO_CAM, "POST", "/api/camera/restart", NULL,
                       body, sizeof(body));

    check(status == 503 && body_has(body, "\"restarted\":false"),
          "camera restart fails cleanly while the device is missing");

    status = http_call(HTTP_PORT_NO_CAM, "GET", "/api/stats", NULL, body,
                       sizeof(body));

    check(status == 200, "stats still answer with no pipeline");

    status = http_call(HTTP_PORT_NO_CAM, "POST", "/api/webrtc/offer", NULL,
                       body, sizeof(body));

    check(status == 413, "offer without a body is rejected");

    stop_server(pid, "SIGTERM ends the failed-camera server cleanly");
}

/*
 * The configuration file that `make install` puts in /etc is part of the
 * shipped product, so it has to load. It is read here unmodified except
 * for the values that would collide with a test run (ports, capture
 * source, mDNS), and then the server is started with it: a typo in that
 * file would have made a fresh install fail to start.
 */
static void test_shipped_config(const char *binary, const char *source_path)
{
    const char *copy_path = "/tmp/camstream_api_shipped.conf";
    const char *log_path = "/tmp/camstream_api_shipped.log";

    printf("test_server_api: shipped configuration file\n");

    FILE *source = fopen(source_path, "rb");

    if (source == NULL) {
        check(0, "the shipped config file is readable");
        return;
    }

    char contents[8192];
    size_t length = fread(contents, 1, sizeof(contents) - 1, source);

    fclose(source);
    contents[length] = 0;

    check(length > 0, "the shipped config file is not empty");

    char overrides[512];

    snprintf(overrides, sizeof(overrides),
             "\n# appended by tests/test_server_api.c\n"
             "source = test\n"
             "mdns = on\n"
             "mdns_port = %u\n"
             "http_port = %u\n"
             "udp_port = %u\n",
             (unsigned) MDNS_PORT_SHIPPED, (unsigned) HTTP_PORT_SHIPPED,
             (unsigned) UDP_PORT_SHIPPED);

    if (write_file(copy_path, contents) != 0 ||
        append_file(copy_path, overrides) != 0) {
        check(0, "the shipped config can be copied for the test");
        return;
    }

    pid_t pid = spawn_server(binary, copy_path, log_path, HTTP_PORT_SHIPPED);

    check(pid > 0, "the server starts with the shipped config file");

    if (pid <= 0) {
        return;
    }

    char body[RESPONSE_MAX];
    int status = http_call(HTTP_PORT_SHIPPED, "GET", "/api/status", NULL, body,
                           sizeof(body));

    check(status == 200 && body_has(body, "\"state\""),
          "the shipped config file yields a serving server");

    /*
     * The responder configured in the shipped file must show up in the
     * status payload, which is where the page reads the name it prints.
     */
    check(body_has(body, "\"mdns\":{\"enabled\":true") &&
          body_has(body, "\"url\":\"http://camstream.local:"),
          "the shipped config enables mDNS and reports the name");

    char log[8192];
    size_t log_length = 0;

    if (read_file(log_path, log, sizeof(log), &log_length) == 0) {
        check(strstr(log, "empty key or value") == NULL &&
              strstr(log, "unknown key") == NULL,
              "no configuration error in the log");
    } else {
        check(0, "the server log of the shipped config run is readable");
    }

    stop_server(pid, "the shipped config run shuts down cleanly");
}

int main(int argc, char **argv)
{
    const char *binary = argc > 1 ? argv[1] : "build/camstream";
    const char *shipped_config = argc > 2 ? argv[2] : "config/camstream.conf";

    signal(SIGPIPE, SIG_IGN);

    printf("test_server_api: binary %s\n", binary);

    if (access(binary, X_OK) != 0) {
        printf("test_server_api: %s is not executable: %s\n", binary,
               strerror(errno));
        return 1;
    }

    test_management_interface(binary);
    test_concurrent_viewers(binary);
    test_camera_failure_keeps_http_up(binary);
    test_shipped_config(binary, shipped_config);

    if (g_failures != 0) {
        printf("test_server_api: %d check(s) failed\n", g_failures);
        return 1;
    }

    printf("test_server_api: PASS\n");

    return 0;
}
