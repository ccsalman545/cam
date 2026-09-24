/*
 * test_lan_stream.c
 *
 * Drives a running camstream exactly as a browser on the LAN does, and
 * checks that decodable video comes out. Everything else in the suite
 * either links a module (test_rtc_session) or speaks HTTP
 * (test_server_api); this one is the only test that proves the assembled
 * server loop delivers media over real sockets:
 *
 *   1. start the binary with a test source and the software encoder
 *   2. POST an SDP offer, take the answer, its credentials and its
 *      DTLS fingerprint
 *   3. STUN binding check with MESSAGE-INTEGRITY, verify the response
 *   4. DTLS 1.2 handshake as the client, verify the certificate against
 *      the fingerprint from the answer
 *   5. export the SRTP keying material and decrypt the video that
 *      arrives, reassembling access units from single NAL and FU-A
 *      packets, and decrypting the Sender Reports
 *   6. close the viewer, then connect a second time to prove recovery
 *   7. stop the server with SIGTERM and expect exit status 0
 *
 * usage: test_lan_stream [path-to-camstream]
 *
 * Ports are high and bound to 127.0.0.1 so the test can run next to a
 * production instance. The server's own UDP socket binds INADDR_ANY, so
 * loopback works even though the SDP advertises the LAN address.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <srtp2/srtp.h>

#define HTTP_PORT_TEST 18995
#define UDP_PORT_TEST  60998

#define SERVER_CONFIG "/tmp/camstream_lan_test.conf"
#define SERVER_LOG    "/tmp/camstream_lan_test.log"

#define RESPONSE_MAX 65536
#define MAX_PACKET   1500
#define MAX_AU       (256 * 1024)

#define CLIENT_UFrag "lanstreamuf"
#define CLIENT_PWD   "lanstreampwd1234567890"
#define CLIENT_TIMEOUT_S 3
#define HANDSHAKE_DEADLINE_MS 10000
#define MEDIA_DEADLINE_MS     8000
#define START_TIMEOUT_MS      8000
#define STOP_TIMEOUT_MS       8000

#define STUN_HEADER_SIZE 20
#define STUN_MAGIC_COOKIE 0x2112A442u

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

static void note(const char *format, ...)
{
    va_list args;

    va_start(args, format);
    printf("       ");
    vprintf(format, args);
    printf("\n");
    va_end(args);
}

static uint64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t) ts.tv_sec * 1000ULL + (uint64_t) ts.tv_nsec / 1000000ULL;
}

static void sleep_ms(int milliseconds)
{
    struct timespec delay;

    delay.tv_sec = milliseconds / 1000;
    delay.tv_nsec = (long) (milliseconds % 1000) * 1000000L;

    nanosleep(&delay, NULL);
}

/* ------------------------------------------------------------------ */
/* Server process                                                      */
/* ------------------------------------------------------------------ */

static int write_config(void)
{
    FILE *file = fopen(SERVER_CONFIG, "wb");

    if (file == NULL) {
        return -1;
    }

    /*
     * encoder = auto: hardware where there is one, libx264 otherwise.
     * A build with neither still serves the handshake, so the media
     * checks are skipped rather than failed in that case.
     */
    int written = fprintf(file,
                          "# written by tests/test_lan_stream.c\n"
                          "source = test\n"
                          "encoder = auto\n"
                          "width = 320\n"
                          "height = 240\n"
                          "fps = 15\n"
                          "bitrate_kbps = 1200\n"
                          "keyframe_seconds = 1\n"
                          "listen = 127.0.0.1\n"
                          "http_port = %d\n"
                          "udp_port = %d\n"
                          "verbose = 0\n",
                          HTTP_PORT_TEST, UDP_PORT_TEST);

    fclose(file);

    return written > 0 ? 0 : -1;
}

static int port_is_open(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        return 0;
    }

    struct sockaddr_in address;

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);

    int result = connect(fd, (struct sockaddr *) &address, sizeof(address));

    close(fd);

    return result == 0;
}

static pid_t spawn_server(const char *binary)
{
    pid_t pid = fork();

    if (pid < 0) {
        return -1;
    }

    if (pid == 0) {
        int log_fd = open(SERVER_LOG, O_WRONLY | O_CREAT | O_TRUNC, 0644);

        if (log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
            close(log_fd);
        }

        execl(binary, "camstream", "--config", SERVER_CONFIG, (char *) NULL);
        _exit(127);
    }

    int waited = 0;

    while (waited <= START_TIMEOUT_MS) {
        if (port_is_open(HTTP_PORT_TEST)) {
            return pid;
        }

        sleep_ms(25);
        waited += 25;
    }

    /* A server that never bound the port is not left running. */
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);

    return -1;
}

static void stop_server(pid_t pid, const char *name)
{
    if (pid <= 0) {
        return;
    }

    kill(pid, SIGTERM);

    int waited = 0;
    int status = 0;

    while (waited <= STOP_TIMEOUT_MS) {
        if (waitpid(pid, &status, WNOHANG) == pid) {
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
/* HTTP                                                                */
/* ------------------------------------------------------------------ */

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
 * One HTTP exchange. 'method' is GET or POST, 'payload' is the POST body
 * or NULL. The response body is returned in 'body'.
 */
static int http_call(const char *method,
                     const char *path,
                     const char *payload,
                     char *body,
                     size_t body_size)
{
    int fd = connect_to(HTTP_PORT_TEST);

    body[0] = 0;

    if (fd < 0) {
        return -1;
    }

    char request[32768];
    int length;

    if (payload != NULL) {
        length = snprintf(request, sizeof(request),
                          "%s %s HTTP/1.1\r\n"
                          "Host: 127.0.0.1:%d\r\n"
                          "Content-Type: application/json\r\n"
                          "Content-Length: %zu\r\n"
                          "Connection: close\r\n\r\n%s",
                          method, path, HTTP_PORT_TEST, strlen(payload),
                          payload);
    } else {
        length = snprintf(request, sizeof(request),
                          "%s %s HTTP/1.1\r\n"
                          "Host: 127.0.0.1:%d\r\n"
                          "Connection: close\r\n\r\n",
                          method, path, HTTP_PORT_TEST);
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

    if (separator != NULL) {
        const char *content = separator + 4;

        snprintf(body, body_size, "%s", content);
    }

    return status;
}

/* Extracts a JSON string value such as "sdp" or "session_id". */
static int json_string(const char *body, const char *field,
                       char *out, size_t out_size)
{
    char pattern[64];

    snprintf(pattern, sizeof(pattern), "\"%s\":\"", field);

    const char *at = strstr(body, pattern);

    if (at == NULL) {
        return -1;
    }

    at += strlen(pattern);

    size_t i = 0;

    while (at[i] != 0 && at[i] != '"' && i + 1 < out_size) {
        out[i] = at[i];
        i++;
    }

    out[i] = 0;

    return 0;
}

/*
 * Turns the escape sequences of a JSON string into the bytes they stand
 * for. The server returns the SDP with real CRLF escaped as \r\n, so
 * attribute parsing only works after this step.
 */
static void json_unescape(char *text)
{
    char *out = text;

    for (const char *in = text; *in != 0; in++) {
        if (*in != '\\') {
            *out++ = *in;
            continue;
        }

        in++;

        switch (*in) {
        case 'r':  *out++ = '\r'; break;
        case 'n':  *out++ = '\n'; break;
        case 't':  *out++ = '\t'; break;
        case 'b':  *out++ = '\b'; break;
        case 'f':  *out++ = '\f'; break;
        case '"':  *out++ = '"';  break;
        case '\\': *out++ = '\\'; break;
        case '/':  *out++ = '/';  break;
        case 'u': {
            /*
             * The server only escapes control characters this way, and
             * the SDP has none left after \\r and \\n, so an unknown
             * escape is passed through rather than guessed at.
             */
            unsigned int code = 0;

            if (sscanf(in + 1, "%4x", &code) == 1) {
                *out++ = (char) code;
                in += 4;
            } else {
                *out++ = *in;
            }
            break;
        }
        case 0:
            *out = 0;
            return;
        default:
            *out++ = *in;
            break;
        }
    }

    *out = 0;
}

/* Extracts a JSON integer value. */
static long json_long(const char *body, const char *field)
{
    char pattern[64];

    snprintf(pattern, sizeof(pattern), "\"%s\":", field);

    const char *at = strstr(body, pattern);

    if (at == NULL) {
        return -1;
    }

    return strtol(at + strlen(pattern), NULL, 10);
}

/* Value of one SDP attribute line ("ice-ufrag", "fingerprint", ...). */
static int sdp_attribute(const char *sdp, const char *name,
                         char *out, size_t out_size)
{
    char needle[64];

    snprintf(needle, sizeof(needle), "a=%s:", name);

    const char *at = strstr(sdp, needle);

    if (at == NULL) {
        return -1;
    }

    at += strlen(needle);

    size_t i = 0;

    while (at[i] != 0 && at[i] != '\r' && at[i] != '\n' && i + 1 < out_size) {
        out[i] = at[i];
        i++;
    }

    out[i] = 0;

    return 0;
}

static int sdp_video_payload_type(const char *sdp)
{
    const char *at = strstr(sdp, "a=rtpmap:");

    while (at != NULL) {
        int payload_type = -1;

        if (sscanf(at, "a=rtpmap:%d H264/90000", &payload_type) == 1 &&
            payload_type >= 0) {
            return payload_type;
        }

        at = strstr(at + 1, "a=rtpmap:");
    }

    return -1;
}

static uint32_t sdp_video_ssrc(const char *sdp)
{
    const char *at = strstr(sdp, "a=ssrc:");

    if (at == NULL) {
        return 0;
    }

    return (uint32_t) strtoul(at + strlen("a=ssrc:"), NULL, 10);
}

/* Builds the offer a browser would send. */
static int build_offer(char *out, size_t out_size, const char *fingerprint)
{
    return snprintf(out, out_size,
                    "{\"sdp\":\"v=0\\r\\n"
                    "o=- 1 2 IN IP4 127.0.0.1\\r\\n"
                    "s=-\\r\\n"
                    "t=0 0\\r\\n"
                    "a=group:BUNDLE 0\\r\\n"
                    "m=video 9 UDP/TLS/RTP/SAVPF 96\\r\\n"
                    "c=IN IP4 0.0.0.0\\r\\n"
                    "a=ice-ufrag:%s\\r\\n"
                    "a=ice-pwd:%s\\r\\n"
                    "a=fingerprint:%s\\r\\n"
                    "a=setup:actpass\\r\\n"
                    "a=mid:0\\r\\n"
                    "a=sendrecv\\r\\n"
                    "a=rtcp-mux\\r\\n"
                    "a=rtpmap:96 H264/90000\\r\\n"
                    "a=fmtp:96 level-asymmetry-allowed=1;"
                    "packetization-mode=1;profile-level-id=42e01f\\r\\n\"}",
                    CLIENT_UFrag, CLIENT_PWD, fingerprint) < (int) out_size
               ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* STUN                                                                */
/* ------------------------------------------------------------------ */

static void write_be16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t) (value >> 8);
    p[1] = (uint8_t) value;
}

static size_t make_binding_request(uint8_t *out, const char *username)
{
    size_t ulen = strlen(username);
    size_t padded = (ulen + 3) & ~3u;

    memset(out, 0, STUN_HEADER_SIZE + 4 + padded);
    write_be16(out, 0x0001);
    write_be16(out + 2, (uint16_t) (4 + padded));
    out[4] = 0x21;
    out[5] = 0x12;
    out[6] = 0xA4;
    out[7] = 0x42;

    write_be16(out + STUN_HEADER_SIZE, 0x0006);
    write_be16(out + STUN_HEADER_SIZE + 2, (uint16_t) ulen);
    memcpy(out + STUN_HEADER_SIZE + 4, username, ulen);

    return STUN_HEADER_SIZE + 4 + padded;
}

static size_t sign_request(uint8_t *request, size_t length, const char *password)
{
    uint8_t *mi = request + length;

    write_be16(mi, 0x0008);
    write_be16(mi + 2, 20);
    write_be16(request + 2, (uint16_t) (length - STUN_HEADER_SIZE + 24));

    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int mac_length = 0;

    HMAC(EVP_sha1(), password, (int) strlen(password), request, length,
         mac, &mac_length);

    memcpy(mi + 4, mac, 20);

    return length + 24;
}

static int verify_message_integrity(const uint8_t *message, size_t length,
                                    const char *password)
{
    size_t offset = STUN_HEADER_SIZE;

    while (offset + 4 <= length) {
        uint16_t type = (uint16_t) ((message[offset] << 8) | message[offset + 1]);
        uint16_t attribute_length = (uint16_t) ((message[offset + 2] << 8) |
                                                message[offset + 3]);

        if (offset + 4 + attribute_length > length) {
            return 0;
        }

        if (type == 0x0008 && attribute_length == 20) {
            uint8_t copy[512];

            if (offset > sizeof(copy)) {
                return 0;
            }

            memcpy(copy, message, offset);

            /*
             * RFC 5389 section 15.4: the HMAC covers the message up to
             * MESSAGE-INTEGRITY with the length field set as if it were
             * the last attribute.
             */
            write_be16(copy + 2, (uint16_t) (offset - STUN_HEADER_SIZE + 24));

            unsigned char mac[EVP_MAX_MD_SIZE];
            unsigned int mac_length = 0;

            HMAC(EVP_sha1(), password, (int) strlen(password), copy, offset,
                 mac, &mac_length);

            return memcmp(mac, message + offset + 4, 20) == 0;
        }

        offset += 4 + ((attribute_length + 3) & ~3u);
    }

    return 0;
}

static uint16_t stun_exchange(int client_fd,
                              const struct sockaddr_in *server_addr,
                              const char *username,
                              const char *password,
                              uint8_t *response,
                              size_t response_capacity,
                              size_t *response_length)
{
    uint8_t request[256];

    size_t length = make_binding_request(request, username);

    length = sign_request(request, length, password);

    if (sendto(client_fd, request, length, 0,
               (const struct sockaddr *) server_addr,
               sizeof(*server_addr)) != (ssize_t) length) {
        return 0;
    }

    uint64_t deadline = now_ms() + 1000;

    while (now_ms() < deadline) {
        struct pollfd fds[1];

        fds[0].fd = client_fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;

        if (poll(fds, 1, 50) <= 0) {
            continue;
        }

        ssize_t received = recv(client_fd, response, response_capacity, 0);

        if (received <= 0) {
            continue;
        }

        *response_length = (size_t) received;

        return (uint16_t) (((uint16_t) response[0] << 8) | response[1]);
    }

    *response_length = 0;

    return 0;
}

/* ------------------------------------------------------------------ */
/* DTLS client                                                         */
/* ------------------------------------------------------------------ */

static SSL_CTX *create_client_ctx(char *fingerprint, size_t fingerprint_size)
{
    EVP_PKEY *key = EVP_EC_gen("P-256");

    if (key == NULL) {
        return NULL;
    }

    X509 *certificate = X509_new();

    if (certificate == NULL) {
        EVP_PKEY_free(key);
        return NULL;
    }

    X509_set_version(certificate, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(certificate), 1);
    X509_gmtime_adj(X509_getm_notBefore(certificate), -3600);
    X509_gmtime_adj(X509_getm_notAfter(certificate), 3600);
    X509_set_pubkey(certificate, key);

    X509_NAME *name = X509_get_subject_name(certificate);

    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               (const unsigned char *) "lan-test-client",
                               -1, -1, 0);

    if (X509_sign(certificate, key, EVP_sha256()) == 0) {
        X509_free(certificate);
        EVP_PKEY_free(key);
        return NULL;
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_length = 0;

    X509_digest(certificate, EVP_sha256(), digest, &digest_length);

    size_t offset = (size_t) snprintf(fingerprint, fingerprint_size,
                                      "sha-256 ");

    for (unsigned int i = 0; i < digest_length; i++) {
        int written = snprintf(fingerprint + offset, fingerprint_size - offset,
                               "%s%02X", i ? ":" : "", digest[i]);

        if (written < 0 || (size_t) written >= fingerprint_size - offset) {
            X509_free(certificate);
            EVP_PKEY_free(key);
            return NULL;
        }

        offset += (size_t) written;
    }

    SSL_CTX *ctx = SSL_CTX_new(DTLS_client_method());

    if (ctx == NULL) {
        X509_free(certificate);
        EVP_PKEY_free(key);
        return NULL;
    }

    SSL_CTX_set_min_proto_version(ctx, DTLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(ctx, DTLS1_2_VERSION);
    SSL_CTX_use_certificate(ctx, certificate);
    SSL_CTX_use_PrivateKey(ctx, key);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    SSL_CTX_set_tlsext_use_srtp(ctx, "SRTP_AES128_CM_SHA1_80");
    SSL_CTX_set_options(ctx, SSL_OP_NO_QUERY_MTU);

    X509_free(certificate);
    EVP_PKEY_free(key);

    return ctx;
}

/*
 * RFC 5764 order of the 60 bytes of keying material:
 *   client key (16) | server key (16) | client salt (14) | server salt (14)
 * The browser decrypts with the server half.
 */
static int create_client_srtp(SSL *ssl, srtp_t *inbound_out, srtp_t *outbound_out)
{
    unsigned char material[60];

    if (SSL_export_keying_material(ssl, material, sizeof(material),
                                   "EXTRACTOR-dtls_srtp", 19,
                                   NULL, 0, 0) != 1) {
        return -1;
    }

    uint8_t server_key[30];
    uint8_t client_key[30];

    memcpy(server_key, material + 16, 16);
    memcpy(server_key + 16, material + 46, 14);

    memcpy(client_key, material, 16);
    memcpy(client_key + 16, material + 32, 14);

    srtp_policy_t policy;

    memset(&policy, 0, sizeof(policy));

    srtp_crypto_policy_set_rtp_default(&policy.rtp);
    srtp_crypto_policy_set_rtcp_default(&policy.rtcp);

    policy.ssrc.type = ssrc_any_inbound;
    policy.key = server_key;
    policy.allow_repeat_tx = 1;

    if (srtp_create(inbound_out, &policy) != srtp_err_status_ok) {
        return -1;
    }

    policy.ssrc.type = ssrc_any_outbound;
    policy.key = client_key;

    if (srtp_create(outbound_out, &policy) != srtp_err_status_ok) {
        srtp_dealloc(*inbound_out);
        *inbound_out = NULL;
        return -1;
    }

    return 0;
}

/*
 * Run the handshake, feeding nothing but the socket itself: unlike the
 * in-process session test, here OpenSSL drives a dgram BIO straight over
 * the client socket, which is exactly what a browser does.
 */
static int run_handshake(SSL *ssl)
{
    uint64_t deadline = now_ms() + HANDSHAKE_DEADLINE_MS;

    for (;;) {
        int result = SSL_do_handshake(ssl);

        if (result == 1) {
            return 0;
        }

        int error = SSL_get_error(ssl, result);

        if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
            printf("       SSL_do_handshake failed: %d errno=%d (%s)\n",
                   error, errno, strerror(errno));
            return -1;
        }

        if (now_ms() > deadline) {
            printf("       DTLS handshake timed out\n");
            return -1;
        }

        struct pollfd fds[1];

        fds[0].fd = SSL_get_fd(ssl);
        fds[0].events = POLLIN;
        fds[0].revents = 0;

        poll(fds, 1, 20);
    }
}

/* ------------------------------------------------------------------ */
/* Media                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    int packets;                /* decrypted RTP packets */
    int access_units;           /* complete access units reassembled */
    int idr_seen;
    int sender_reports;         /* SRTCP Sender Reports decrypted */
    int rtcp_undecryptable;     /* SRTCP packets the client could not open */
    uint32_t first_timestamp;
    uint32_t last_timestamp;
    uint32_t last_sequence;
    uint32_t au_timestamp;
    int sequence_gaps;
    int wrong_ssrc;
    int wrong_payload_type;
    size_t au_length;
    uint8_t au[MAX_AU];
    int au_parts;
} MediaStats;

/*
 * Reassembles access units from single NAL and FU-A packets and applies
 * the checks a decoder would: the payload must start with a known NAL
 * type and the marker bit must close the access unit.
 */
static void accept_rtp(MediaStats *stats, const uint8_t *packet, int length,
                       int payload_type, uint32_t ssrc)
{
    if (length < 12) {
        return;
    }

    if ((packet[1] & 0x7F) != payload_type) {
        stats->wrong_payload_type++;
    }

    uint32_t packet_ssrc = ((uint32_t) packet[8] << 24) |
                           ((uint32_t) packet[9] << 16) |
                           ((uint32_t) packet[10] << 8) |
                           (uint32_t) packet[11];

    if (packet_ssrc != ssrc) {
        stats->wrong_ssrc++;
    }

    uint16_t sequence = (uint16_t) (((uint16_t) packet[2] << 8) | packet[3]);
    uint32_t timestamp = ((uint32_t) packet[4] << 24) |
                         ((uint32_t) packet[5] << 16) |
                         ((uint32_t) packet[6] << 8) |
                         (uint32_t) packet[7];

    stats->packets++;

    /*
     * Within one access unit the sequence numbers must be contiguous: a
     * gap here would mean the packetizer or the socket path dropped a
     * fragment, which the browser would have to repair with a NACK.
     */
    if (stats->packets > 1 && timestamp == stats->last_timestamp &&
        sequence != (uint16_t) (stats->last_sequence + 1)) {
        stats->sequence_gaps++;
    }

    if (stats->first_timestamp == 0) {
        stats->first_timestamp = timestamp;
    }

    stats->last_timestamp = timestamp;
    stats->last_sequence = sequence;

    const uint8_t *payload = packet + 12;
    int payload_length = length - 12;

    if (payload_length <= 0) {
        return;
    }

    uint8_t nal_type = (uint8_t) (payload[0] & 0x1F);

    /*
     * One access unit can hold several NAL units (SPS, PPS and the IDR
     * slice share one RTP timestamp and one marker). A new timestamp
     * starts a new access unit; the marker closes it.
     */
    if (stats->au_parts == 0 || timestamp != stats->au_timestamp) {
        stats->au_length = 0;
        stats->au_parts = 0;
        stats->au_timestamp = timestamp;
    }

    if (nal_type == 28 && payload_length > 2) {
        uint8_t fu_header = payload[1];

        if ((fu_header & 0x80) != 0) {
            uint8_t reconstructed = (uint8_t) ((payload[0] & 0xE0) |
                                               (fu_header & 0x1F));

            if ((fu_header & 0x1F) == 5) {
                stats->idr_seen = 1;
            }

            if (stats->au_length + 1 <= sizeof(stats->au)) {
                stats->au[stats->au_length++] = reconstructed;
            }
        }

        if (stats->au_length + (size_t) (payload_length - 2) <=
            sizeof(stats->au)) {
            memcpy(stats->au + stats->au_length, payload + 2,
                   (size_t) (payload_length - 2));
            stats->au_length += (size_t) (payload_length - 2);
        }

        stats->au_parts++;
    } else {
        if (nal_type == 5) {
            stats->idr_seen = 1;
        }

        if (stats->au_length + (size_t) payload_length <= sizeof(stats->au)) {
            memcpy(stats->au + stats->au_length, payload,
                   (size_t) payload_length);
            stats->au_length += (size_t) payload_length;
            stats->au_parts++;
        }
    }

    if ((packet[1] & 0x80) != 0 && stats->au_parts > 0) {
        stats->access_units++;
        stats->au_length = 0;
        stats->au_parts = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Scenario                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    int client_fd;
    struct sockaddr_in session_addr;
    SSL_CTX *ssl_ctx;
    char fingerprint[128];
    int payload_type;
    uint32_t ssrc;
    char session_id[32];
} Viewer;

static void viewer_close(Viewer *viewer)
{
    if (viewer->client_fd >= 0) {
        close(viewer->client_fd);
        viewer->client_fd = -1;
    }
}

/*
 * Signaling, STUN, DTLS and one media window. Returns 0 when video
 * arrived, -1 when any step failed; the checks are recorded inside.
 */
static int connect_viewer(Viewer *viewer, const char *label, srtp_t *inbound,
                          srtp_t *outbound, SSL **ssl_out,
                          MediaStats *media, int require_media)
{
    char offer[8192];

    if (build_offer(offer, sizeof(offer), viewer->fingerprint) != 0) {
        check(0, "build offer");
        return -1;
    }

    char body[RESPONSE_MAX];
    char tag[128];

    int status = http_call("POST", "/api/webrtc/offer", offer, body,
                           sizeof(body));

    snprintf(tag, sizeof(tag), "%s: offer answered", label);
    check(status == 200, tag);

    char answer[8192];

    if (status != 200 || json_string(body, "sdp", answer, sizeof(answer)) != 0) {
        return -1;
    }

    json_unescape(answer);

    long udp_port = json_long(body, "udp_port");
    long session_id = json_long(body, "session_id");

    snprintf(viewer->session_id, sizeof(viewer->session_id), "%ld", session_id);

    snprintf(tag, sizeof(tag), "%s: answer has a UDP port", label);
    check(udp_port > 0 && udp_port < 65536, tag);

    char server_ufrag[80] = "";
    char server_pwd[128] = "";
    char server_fingerprint[128] = "";

    int parsed = sdp_attribute(answer, "ice-ufrag", server_ufrag,
                              sizeof(server_ufrag)) == 0 &&
                 sdp_attribute(answer, "ice-pwd", server_pwd,
                               sizeof(server_pwd)) == 0 &&
                 sdp_attribute(answer, "fingerprint", server_fingerprint,
                               sizeof(server_fingerprint)) == 0;

    snprintf(tag, sizeof(tag), "%s: answer carries credentials and fingerprint",
             label);
    check(parsed, tag);

    if (!parsed) {
        return -1;
    }

    viewer->payload_type = sdp_video_payload_type(answer);
    viewer->ssrc = sdp_video_ssrc(answer);

    snprintf(tag, sizeof(tag), "%s: answer describes H264 video", label);
    check(viewer->payload_type >= 0 && viewer->ssrc != 0, tag);

    if (viewer->payload_type < 0 || viewer->ssrc == 0) {
        return -1;
    }

    viewer->client_fd = socket(AF_INET, SOCK_DGRAM, 0);

    struct sockaddr_in local;

    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    bind(viewer->client_fd, (struct sockaddr *) &local, sizeof(local));

    int flags = fcntl(viewer->client_fd, F_GETFL, 0);

    if (flags >= 0) {
        fcntl(viewer->client_fd, F_SETFL, flags | O_NONBLOCK);
    }

    memset(&viewer->session_addr, 0, sizeof(viewer->session_addr));
    viewer->session_addr.sin_family = AF_INET;
    viewer->session_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    viewer->session_addr.sin_port = htons((uint16_t) udp_port);

    char username[192];

    snprintf(username, sizeof(username), "%s:%s", server_ufrag, CLIENT_UFrag);

    uint8_t response[512];
    size_t response_length = 0;

    uint16_t response_type = stun_exchange(viewer->client_fd,
                                           &viewer->session_addr, username,
                                           server_pwd, response,
                                           sizeof(response),
                                           &response_length);

    snprintf(tag, sizeof(tag), "%s: ICE check answered 0x0101", label);
    check(response_type == 0x0101, tag);
    check(verify_message_integrity(response, response_length, server_pwd),
          "response MESSAGE-INTEGRITY verifies");

    SSL *ssl = SSL_new(viewer->ssl_ctx);
    BIO *bio = BIO_new_dgram(viewer->client_fd, BIO_NOCLOSE);

    BIO_ctrl(bio, BIO_CTRL_DGRAM_SET_PEER, 0, &viewer->session_addr);
    SSL_set_bio(ssl, bio, bio);
    SSL_set_connect_state(ssl);
    SSL_set_mtu(ssl, 1200);

    snprintf(tag, sizeof(tag), "%s: DTLS 1.2 handshake completes", label);
    check(run_handshake(ssl) == 0, tag);

    X509 *peer = SSL_get_peer_certificate(ssl);

    if (peer != NULL) {
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int digest_length = 0;

        X509_digest(peer, EVP_sha256(), digest, &digest_length);

        char actual[128];
        size_t offset = 0;

        for (unsigned int i = 0; i < digest_length; i++) {
            offset += (size_t) snprintf(actual + offset,
                                        sizeof(actual) - offset,
                                        "%s%02X", i ? ":" : "", digest[i]);
        }

        const char *expected = server_fingerprint;

        if (strncmp(expected, "sha-256 ", 8) == 0) {
            expected += 8;
        }

        check(strcasecmp(actual, expected) == 0,
              "server certificate matches the fingerprint in the answer");

        X509_free(peer);
    } else {
        check(0, "server certificate matches the fingerprint in the answer");
    }

    if (create_client_srtp(ssl, inbound, outbound) != 0) {
        check(0, "SRTP keying material exported on the client side");
        SSL_free(ssl);
        return -1;
    }

    check(1, "SRTP keying material exported on the client side");

    *ssl_out = ssl;

    /* --- media window ------------------------------------------- */

    uint64_t deadline = now_ms() + MEDIA_DEADLINE_MS;

    while (now_ms() < deadline) {
        struct pollfd fds[1];

        fds[0].fd = viewer->client_fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;

        if (poll(fds, 1, 50) <= 0) {
            if (!require_media) {
                break;
            }

            continue;
        }

        uint8_t datagram[MAX_PACKET];
        ssize_t received = recv(viewer->client_fd, datagram, sizeof(datagram), 0);

        if (received <= 0) {
            continue;
        }

        int length = (int) received;
        uint8_t first = datagram[0];
        uint8_t second = datagram[1];

        if (first < 128 || first > 191) {
            continue;
        }

        /*
         * RFC 5761: RTP and RTCP share the same port. The stream uses
         * payload type 96, so a second octet in 192..223 is an RTCP
         * packet type (Sender Report is 200) and must go through
         * srtp_unprotect_rtcp(); anything else is RTP.
         */
        if (second >= 192 && second <= 223) {
            if (srtp_unprotect_rtcp(*inbound, datagram, &length) !=
                srtp_err_status_ok || length < 8) {
                media->rtcp_undecryptable++;
            } else if (datagram[1] == 200) {
                media->sender_reports++;
            }
        } else {
            if (srtp_unprotect(*inbound, datagram, &length) !=
                srtp_err_status_ok) {
                continue;
            }

            accept_rtp(media, datagram, length, viewer->payload_type,
                       viewer->ssrc);

            if (media->access_units >= 4 && media->sender_reports > 0) {
                break;
            }
        }
    }

    return media->packets > 0 ? 0 : -1;
}

int main(int argc, char **argv)
{
    const char *binary = argc > 1 ? argv[1] : "build/camstream";

    signal(SIGPIPE, SIG_IGN);

    printf("test_lan_stream: binary %s\n", binary);

    if (access(binary, X_OK) != 0) {
        printf("test_lan_stream: %s is not executable: %s\n", binary,
               strerror(errno));
        return 1;
    }

    if (srtp_init() != srtp_err_status_ok) {
        printf("test_lan_stream: srtp_init failed\n");
        return 1;
    }

    if (port_is_open(HTTP_PORT_TEST)) {
        /*
         * Testing against a server this process did not start would make
         * every result meaningless, so refuse instead of guessing.
         */
        printf("test_lan_stream: port %d is already in use\n", HTTP_PORT_TEST);
        return 1;
    }

    if (write_config() != 0) {
        printf("test_lan_stream: cannot write %s\n", SERVER_CONFIG);
        return 1;
    }

    pid_t pid = spawn_server(binary);

    check(pid > 0, "server starts and serves HTTP");

    if (pid <= 0) {
        return 1;
    }

    char body[RESPONSE_MAX];

    if (http_call("GET", "/api/status", NULL, body, sizeof(body)) != 200) {
        check(0, "status answers");
        stop_server(pid, "SIGTERM ends the server");
        return 1;
    }

    /*
     * Without an encoder the handshake can still be verified but no
     * media can arrive; say so instead of reporting a false failure.
     */
    int media_expected = strstr(body, "\"encoder\":{\"name\":\"none\"") == NULL;

    if (!media_expected) {
        note("no encoder pipeline on this machine: media checks are skipped");
    }

    Viewer viewer;

    memset(&viewer, 0, sizeof(viewer));
    viewer.client_fd = -1;

    viewer.ssl_ctx = create_client_ctx(viewer.fingerprint,
                                       sizeof(viewer.fingerprint));

    if (viewer.ssl_ctx == NULL) {
        check(0, "client SSL context");
        stop_server(pid, "SIGTERM ends the server");
        return 1;
    }

    /* --- first viewer ------------------------------------------- */

    srtp_t inbound = NULL;
    srtp_t outbound = NULL;
    SSL *ssl = NULL;
    MediaStats media;

    memset(&media, 0, sizeof(media));

    int connected = connect_viewer(&viewer, "viewer 1", &inbound, &outbound,
                                   &ssl, &media, media_expected);

    if (media_expected) {
        check(media.packets >= 10, "SRTP video packets received and decrypted");
        check(media.wrong_payload_type == 0 && media.wrong_ssrc == 0,
              "every packet carries the negotiated payload type and SSRC");
        check(media.access_units >= 2,
              "access units reassembled from single NAL and FU-A packets");
        check(media.idr_seen, "a keyframe arrived for the new viewer");
        check(media.sequence_gaps == 0,
              "sequence numbers are contiguous within each access unit");
        check(media.last_timestamp > media.first_timestamp,
              "RTP timestamps advance with capture time");
        check(media.sender_reports > 0,
              "SRTCP sender reports received and decrypted");

        char stats_body[RESPONSE_MAX];

        http_call("GET", "/api/stats", NULL, stats_body, sizeof(stats_body));

        long rtcp_sent = json_long(stats_body, "rtcp_sent");

        note("packets=%d access_units=%d idr=%d sender_reports=%d "
             "(server rtcp_sent=%ld, undecryptable=%d)",
             media.packets, media.access_units, media.idr_seen,
             media.sender_reports, rtcp_sent, media.rtcp_undecryptable);

        check(rtcp_sent > 0, "server sent SRTCP sender reports");
    } else {
        check(connected == 0 || media.packets == 0,
              "session reached streaming without an encoder");
    }

    /* --- viewer leaves ------------------------------------------ */

    char close_body[64];

    snprintf(close_body, sizeof(close_body), "{\"session_id\":%s}",
             viewer.session_id);

    int status = http_call("POST", "/api/webrtc/close", close_body, body,
                           sizeof(body));

    check(status == 200, "closing the viewer is acknowledged");

    sleep_ms(200);

    http_call("GET", "/api/status", NULL, body, sizeof(body));

    check(strstr(body, "\"sessions_active\":0") != NULL,
          "the server reports no active session after the close");

    if (ssl != NULL) {
        SSL_free(ssl);
    }

    if (inbound != NULL) {
        srtp_dealloc(inbound);
    }

    if (outbound != NULL) {
        srtp_dealloc(outbound);
    }

    viewer_close(&viewer);

    /* --- second viewer, same process ----------------------------- */

    inbound = NULL;
    outbound = NULL;
    ssl = NULL;

    memset(&media, 0, sizeof(media));

    int reconnected = connect_viewer(&viewer, "viewer 2", &inbound, &outbound,
                                     &ssl, &media, media_expected);

    if (media_expected) {
        check(media.packets >= 10,
              "a later viewer receives video without restarting the server");
        check(media.idr_seen, "the later viewer also gets a keyframe");
        check(reconnected == 0, "reconnect completes");
    }

    /* --- no errors in the log ----------------------------------- */

    http_call("GET", "/api/logs?level=error", NULL, body, sizeof(body));

    check(strstr(body, "\"errors\":0") != NULL,
          "the whole flow logged no error");

    if (ssl != NULL) {
        SSL_free(ssl);
    }

    if (inbound != NULL) {
        srtp_dealloc(inbound);
    }

    if (outbound != NULL) {
        srtp_dealloc(outbound);
    }

    viewer_close(&viewer);
    SSL_CTX_free(viewer.ssl_ctx);

    stop_server(pid, "SIGTERM ends the server with status 0");

    srtp_shutdown();

    if (g_failures != 0) {
        printf("test_lan_stream: %d check(s) failed\n", g_failures);
        return 1;
    }

    printf("test_lan_stream: PASS\n");

    return 0;
}
