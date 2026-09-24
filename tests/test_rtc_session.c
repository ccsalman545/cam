#define _POSIX_C_SOURCE 200809L

/*
 * test_rtc_session.c
 *
 * End to end test of one viewer session over loopback. The test plays
 * the browser side: it builds the offer, sends a real STUN binding
 * check, performs a real DTLS 1.2 handshake as the client, exports the
 * SRTP keying material and decrypts the media that camstream sends.
 *
 * Covered here:
 *   1. session creation through the public API and the SDP answer
 *   2. authenticated STUN check accepted, forged checks rejected
 *   3. DTLS handshake with the fingerprint from the offer
 *   4. SRTP protected RTP: payload type, sequence, timestamp, marker
 *   5. NAL reassembly of single NAL units and FU-A fragments
 *   6. RTCP NACK answered from the retransmission cache
 *   7. malformed datagrams do not break the session
 *   8. idle teardown through the public tick entry point
 *
 * Everything above runs against the real modules, no stubs.
 *
 *   make test
 */
#include "webrtc_session.h"
#include "ice_lite.h"
#include "log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <srtp2/srtp.h>

#define CLIENT_UFrag "testPeerUfrag"
#define CLIENT_PWD "testPeerPasswordValue0123456789"
#define RTP_PAYLOAD_TYPE 96
#define MAX_CAPTURED_PACKETS 32
#define MAX_PACKET_SIZE 2048

static int failures;

static EVP_PKEY *client_key;
static X509 *client_cert;

static void expect(int condition, const char *what)
{
    if (condition) {
        printf("  ok   %s\n", what);
    } else {
        printf("  FAIL %s\n", what);
        failures++;
    }
}

static uint64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t) ts.tv_sec * 1000ULL + (uint64_t) ts.tv_nsec / 1000000ULL;
}

static void write_be16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t) (value >> 8);
    p[1] = (uint8_t) value;
}

static void write_be32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t) (value >> 24);
    p[1] = (uint8_t) (value >> 16);
    p[2] = (uint8_t) (value >> 8);
    p[3] = (uint8_t) value;
}

/* ------------------------------------------------------------------ */
/* STUN request building (RFC 5389)                                    */
/* ------------------------------------------------------------------ */

static size_t make_binding_request(uint8_t *out, const char *username)
{
    size_t ulen = strlen(username);
    size_t padded = (ulen + 3) & ~3u;

    memset(out, 0, 20 + 4 + padded);

    write_be16(out, 0x0001);
    write_be16(out + 2, (uint16_t) (4 + padded));
    write_be32(out + 4, STUN_MAGIC_COOKIE);

    for (int i = 0; i < 12; i++) {
        out[8 + i] = (uint8_t) (0x40 + i);
    }

    write_be16(out + 20, 0x0006);
    write_be16(out + 22, (uint16_t) ulen);
    memcpy(out + 24, username, ulen);

    return 20 + 4 + padded;
}

/*
 * Append MESSAGE-INTEGRITY and set the message length as if it were
 * present (RFC 5389 section 15.4: the HMAC covers the message up to but
 * not including the MI attribute).
 */
static size_t sign_request(uint8_t *request, size_t length, const char *password)
{
    uint8_t *mi = request + length;

    write_be16(mi, 0x0008);
    write_be16(mi + 2, 20);

    size_t total = length + 24;

    write_be16(request + 2, (uint16_t) (total - STUN_HEADER_SIZE));

    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int mac_length = 0;

    HMAC(EVP_sha1(), password, (int) strlen(password), request, length,
         mac, &mac_length);

    memcpy(mi + 4, mac, 20);

    return total;
}

static int find_attr(const uint8_t *message, size_t length, uint16_t type,
                     const uint8_t **value, size_t *value_length)
{
    size_t end = STUN_HEADER_SIZE +
                 (size_t) (((uint16_t) message[2] << 8) | message[3]);
    size_t offset = STUN_HEADER_SIZE;

    if (end > length) {
        return 0;
    }

    while (offset + 4 <= end) {
        uint16_t attr_type = (uint16_t) (((uint16_t) message[offset] << 8) |
                                         message[offset + 1]);
        uint16_t attr_length = (uint16_t) (((uint16_t) message[offset + 2] << 8) |
                                           message[offset + 3]);

        if (offset + 4 + attr_length > end) {
            return 0;
        }

        if (attr_type == type) {
            if (value != NULL) {
                *value = message + offset + 4;
            }
            if (value_length != NULL) {
                *value_length = attr_length;
            }
            return 1;
        }

        offset += 4 + ((attr_length + 3) & ~3u);
    }

    return 0;
}

static int verify_message_integrity(const uint8_t *message, size_t length,
                                    const char *password)
{
    size_t end = STUN_HEADER_SIZE +
                 (size_t) (((uint16_t) message[2] << 8) | message[3]);
    size_t offset = STUN_HEADER_SIZE;

    if (end > length) {
        return 0;
    }

    while (offset + 4 <= end) {
        uint16_t attr_type = (uint16_t) (((uint16_t) message[offset] << 8) |
                                         message[offset + 1]);
        uint16_t attr_length = (uint16_t) (((uint16_t) message[offset + 2] << 8) |
                                           message[offset + 3]);

        if (attr_type == 0x0008 && attr_length == 20 &&
            offset + 24 <= length) {
            /*
             * RFC 5389 section 15.4: FINGERPRINT is excluded from the
             * HMAC, so the length field in the hashed copy stops at the
             * end of MESSAGE-INTEGRITY.
             */
            uint8_t copy[512];

            if (offset > sizeof(copy)) {
                return 0;
            }

            memcpy(copy, message, offset);
            write_be16(copy + 2, (uint16_t) (offset - STUN_HEADER_SIZE + 24));

            unsigned char mac[EVP_MAX_MD_SIZE];
            unsigned int mac_length = 0;

            HMAC(EVP_sha1(), password, (int) strlen(password), copy,
                 offset, mac, &mac_length);

            return memcmp(mac, message + offset + 4, 20) == 0;
        }

        offset += 4 + ((attr_length + 3) & ~3u);
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Datagram plumbing                                                   */
/* ------------------------------------------------------------------ */

/*
 * Pump the session socket once and return 1 when a datagram for the
 * session was consumed. The session never reads its socket itself, so
 * the test must forward everything it receives, exactly like the server
 * loop does.
 */
static int pump_session(RtcSession *session, int timeout_ms)
{
    struct pollfd fds[2];

    fds[0].fd = rtc_session_fd(session);
    fds[0].events = POLLIN;
    fds[0].revents = 0;

    int ready = poll(fds, 1, timeout_ms);

    if (ready <= 0) {
        return 0;
    }

    uint8_t buffer[MAX_PACKET_SIZE];
    struct sockaddr_storage source;
    socklen_t source_length = sizeof(source);

    ssize_t received = recvfrom(fds[0].fd, buffer, sizeof(buffer), 0,
                                (struct sockaddr *) &source, &source_length);

    if (received <= 0) {
        return 0;
    }

    rtc_session_on_udp(session, buffer, (size_t) received, &source);

    return 1;
}

/* ------------------------------------------------------------------ */
/* Client certificate from the self signed, ECDSA P-256 pair           */
/* ------------------------------------------------------------------ */

static SSL_CTX *create_client_ssl_ctx(char *fingerprint, size_t fingerprint_size,
                                      int offer_use_srtp)
{
    client_key = EVP_EC_gen("P-256");

    if (client_key == NULL) {
        return NULL;
    }

    client_cert = X509_new();

    if (client_cert == NULL) {
        return NULL;
    }

    X509_set_version(client_cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(client_cert), 1);
    X509_gmtime_adj(X509_getm_notBefore(client_cert), -60);
    X509_gmtime_adj(X509_getm_notAfter(client_cert), 3600);
    X509_set_pubkey(client_cert, client_key);

    X509_NAME *name = X509_get_subject_name(client_cert);

    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               (const unsigned char *) "camstream test client",
                               -1, -1, 0);
    X509_set_issuer_name(client_cert, name);

    if (X509_sign(client_cert, client_key, EVP_sha256()) == 0) {
        return NULL;
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_length = 0;

    X509_digest(client_cert, EVP_sha256(), digest, &digest_length);

    size_t offset = (size_t) snprintf(fingerprint, fingerprint_size,
                                      "sha-256 ");

    for (unsigned int i = 0; i < digest_length; i++) {
        int written = snprintf(fingerprint + offset, fingerprint_size - offset,
                               "%s%02X", i ? ":" : "", digest[i]);

        if (written < 0 || (size_t) written >= fingerprint_size - offset) {
            return NULL;
        }

        offset += (size_t) written;
    }

    SSL_CTX *ctx = SSL_CTX_new(DTLS_client_method());

    if (ctx == NULL) {
        return NULL;
    }

    SSL_CTX_set_min_proto_version(ctx, DTLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(ctx, DTLS1_2_VERSION);
    SSL_CTX_use_certificate(ctx, client_cert);
    SSL_CTX_use_PrivateKey(ctx, client_key);

    if (SSL_CTX_check_private_key(ctx) != 1) {
        SSL_CTX_free(ctx);
        return NULL;
    }

    /* WebRTC peers are verified by SDP fingerprint, not by a CA. */
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);

    /*
     * A real browser always offers this; the second scenario omits it
     * to prove the server refuses a peer that never agreed on a profile.
     */
    if (offer_use_srtp) {
        SSL_CTX_set_tlsext_use_srtp(ctx, "SRTP_AES128_CM_SHA1_80");
    }

    SSL_CTX_set_options(ctx, SSL_OP_NO_QUERY_MTU);

    return ctx;
}

/*
 * Drive the handshake from the client side, feeding the session socket
 * in between, until OpenSSL reports success or the deadline passes.
 */
static int run_handshake(RtcSession *session, int client_fd, SSL *ssl)
{
    uint64_t deadline = now_ms() + 10000;

    for (;;) {
        int result = SSL_do_handshake(ssl);

        if (result == 1) {
            return 0;
        }

        int error = SSL_get_error(ssl, result);

        if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
            fprintf(stderr, "  SSL_do_handshake failed: %d errno=%d (%s)\n",
                    error, errno, strerror(errno));
            ERR_print_errors_fp(stderr);
            return -1;
        }

        if (now_ms() > deadline) {
            fprintf(stderr, "  DTLS handshake timed out\n");
            return -1;
        }

        struct pollfd fds[2];

        fds[0].fd = client_fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        fds[1].fd = rtc_session_fd(session);
        fds[1].events = POLLIN;
        fds[1].revents = 0;

        int timeout = rtc_session_dtls_timeout_ms(session);

        if (timeout < 0 || timeout > 50) {
            timeout = 50;
        }

        poll(fds, 2, timeout);

        if (fds[1].revents & POLLIN) {
            uint8_t buffer[MAX_PACKET_SIZE];
            struct sockaddr_storage source;
            socklen_t source_length = sizeof(source);

            ssize_t received = recvfrom(fds[1].fd, buffer, sizeof(buffer), 0,
                                        (struct sockaddr *) &source,
                                        &source_length);

            if (received > 0) {
                rtc_session_on_udp(session, buffer, (size_t) received, &source);
            }
        }

        rtc_session_tick(session, now_ms());
    }
}

/* ------------------------------------------------------------------ */
/* Client side SRTP using the exported key material                    */
/* ------------------------------------------------------------------ */

/*
 * RFC 5764: the 60 byte exporter output is
 *   client key (16) | server key (16) | client salt (14) | server salt (14)
 * The client decrypts with the server half.
 */
static int create_client_srtp(SSL *ssl, srtp_t *inbound_out,
                              srtp_t *outbound_out)
{
    uint8_t material[60];

    if (SSL_export_keying_material(ssl, material, sizeof(material),
                                   "EXTRACTOR-dtls_srtp", 19,
                                   NULL, 0, 0) != 1) {
        return -1;
    }

    uint8_t server_key[30];
    uint8_t client_key_material[30];

    memcpy(server_key, material + 16, 16);
    memcpy(server_key + 16, material + 46, 14);

    memcpy(client_key_material, material, 16);
    memcpy(client_key_material + 16, material + 32, 14);

    srtp_policy_t policy;

    memset(&policy, 0, sizeof(policy));
    srtp_crypto_policy_set_rtp_default(&policy.rtp);
    srtp_crypto_policy_set_rtcp_default(&policy.rtcp);
    policy.ssrc.type = ssrc_any_inbound;
    policy.key = server_key;

    if (srtp_create(inbound_out, &policy) != srtp_err_status_ok) {
        return -1;
    }

    /*
     * Feedback the browser sends (RTCP) is protected with the client
     * write keys, so the test needs that direction as well.
     */
    policy.ssrc.type = ssrc_any_outbound;
    policy.key = client_key_material;

    if (srtp_create(outbound_out, &policy) != srtp_err_status_ok) {
        srtp_dealloc(*inbound_out);
        *inbound_out = NULL;
        return -1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Test access unit                                                    */
/* ------------------------------------------------------------------ */

static const uint8_t sps[] = { 0x67, 0x42, 0xE0, 0x1F, 0xD9, 0x00, 0x50, 0x05 };
static const uint8_t pps[] = { 0x68, 0xCE, 0x3C, 0x80 };
static const uint8_t slice_small[] = { 0x41, 0x9A, 0x22, 0x11, 0x00, 0x04 };

/* 3000 bytes force FU-A fragmentation (RTP budget is 1200 bytes). */
static uint8_t slice_large[3002];

static size_t build_access_unit(uint8_t *out, size_t capacity)
{
    size_t offset = 0;

    const struct {
        const uint8_t *data;
        size_t size;
    } nals[] = {
        { sps, sizeof(sps) },
        { pps, sizeof(pps) },
        { slice_large, sizeof(slice_large) },
        { slice_small, sizeof(slice_small) }
    };

    memset(slice_large, 0x65, sizeof(slice_large));

    for (size_t i = 0; i < sizeof(nals) / sizeof(nals[0]); i++) {
        if (offset + nals[i].size + 4 > capacity) {
            return 0;
        }

        out[offset++] = 0;
        out[offset++] = 0;
        out[offset++] = 0;
        out[offset++] = 1;
        memcpy(out + offset, nals[i].data, nals[i].size);
        offset += nals[i].size;
    }

    return offset;
}

/*
 * Reassemble the RTP payloads of one access unit back into Annex-B.
 * Returns the number of bytes written, 0 on a malformed sequence.
 */
/* 'packets' is only read; it is not const because qualifying an
 * array-of-arrays parameter is not valid ISO C before C23. */
static size_t reassemble(uint8_t packets[][MAX_PACKET_SIZE],
                         const size_t *lengths,
                         int count,
                         uint8_t *out,
                         size_t capacity)
{
    size_t offset = 0;
    int fu_active = 0;

    for (int i = 0; i < count; i++) {
        if (lengths[i] < 13) {
            return 0;
        }

        const uint8_t *payload = packets[i] + 12;
        size_t payload_length = lengths[i] - 12;
        uint8_t nal_type = payload[0] & 0x1F;

        if (nal_type == 28) {
            if (payload_length < 2) {
                return 0;
            }

            uint8_t fu_header = payload[1];
            int start = (fu_header & 0x80) != 0;
            int end = (fu_header & 0x40) != 0;
            uint8_t reconstructed = (uint8_t) ((payload[0] & 0xE0) |
                                               (fu_header & 0x1F));

            if (start) {
                if (offset + 5 > capacity) {
                    return 0;
                }

                out[offset++] = 0;
                out[offset++] = 0;
                out[offset++] = 0;
                out[offset++] = 1;
                out[offset++] = reconstructed;
                fu_active = 1;
            } else if (!fu_active) {
                return 0;
            }

            size_t fragment = payload_length - 2;

            if (offset + fragment > capacity) {
                return 0;
            }

            memcpy(out + offset, payload + 2, fragment);
            offset += fragment;

            if (end) {
                fu_active = 0;
            }
        } else {
            if (fu_active) {
                return 0;
            }

            if (offset + 4 + payload_length > capacity) {
                return 0;
            }

            out[offset++] = 0;
            out[offset++] = 0;
            out[offset++] = 0;
            out[offset++] = 1;
            memcpy(out + offset, payload, payload_length);
            offset += payload_length;
        }
    }

    return fu_active ? 0 : offset;
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

static uint16_t reserve_free_udp_port(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (fd < 0) {
        return 0;
    }

    struct sockaddr_in addr;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
        close(fd);
        return 0;
    }

    socklen_t length = sizeof(addr);

    if (getsockname(fd, (struct sockaddr *) &addr, &length) != 0) {
        close(fd);
        return 0;
    }

    uint16_t port = ntohs(addr.sin_port);

    close(fd);

    return port;
}

static int parse_answer_attribute(const char *answer,
                                  const char *name,
                                  char *out,
                                  size_t out_size)
{
    char needle[32];

    snprintf(needle, sizeof(needle), "a=%s:", name);

    const char *pos = strstr(answer, needle);

    if (pos == NULL) {
        return -1;
    }

    pos += strlen(needle);

    size_t i = 0;

    while (pos[i] != 0 && pos[i] != '\r' && pos[i] != '\n' &&
           i + 1 < out_size) {
        out[i] = pos[i];
        i++;
    }

    out[i] = 0;

    return i > 0 ? 0 : -1;
}

/*
 * Send one binding check, forward datagrams into the session until a
 * response shows up on the client socket, and return its type.
 */
static uint16_t stun_exchange(RtcSession *session,
                              int client_fd,
                              const struct sockaddr_in *session_addr,
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
               (const struct sockaddr *) session_addr,
               sizeof(*session_addr)) != (ssize_t) length) {
        return 0;
    }

    uint64_t deadline = now_ms() + 1000;

    while (now_ms() < deadline) {
        pump_session(session, 5);

        struct pollfd fds[1];

        fds[0].fd = client_fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;

        if (poll(fds, 1, 5) <= 0) {
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

/*
 * A peer that never offers the use_srtp extension reaches the end of the
 * TLS handshake but must not be sent media: the keys would be derived
 * from a profile it never agreed to, so the session has to fail where
 * the cause is visible instead of producing undecryptable packets.
 *
 * A browser cannot make this mistake, but a custom client or a broken
 * gateway can, and it is the one path in the DTLS integration that
 * produces silently wrong output when unguarded.
 */
static void run_peer_without_srtp_scenario(void)
{
    /*
     * create_client_ssl_ctx() publishes the peer certificate and key in
     * file scope variables that main() frees at the very end. This
     * scenario runs first and would overwrite those pointers, leaving
     * the first scenario's certificate unreachable, so release the
     * previous pair here.
     */
    EVP_PKEY_free(client_key);
    X509_free(client_cert);
    client_key = NULL;
    client_cert = NULL;

    char fingerprint[128] = "";

    SSL_CTX *ctx = create_client_ssl_ctx(fingerprint, sizeof(fingerprint), 0);

    expect(ctx != NULL, "peer without use_srtp: client context built");

    if (ctx == NULL) {
        return;
    }

    RtcSessionConfig config;

    memset(&config, 0, sizeof(config));

    config.id = 0x22222222;
    config.udp_port = reserve_free_udp_port();
    config.candidate_count = 1;

    snprintf(config.candidate_ips[0], sizeof(config.candidate_ips[0]),
             "127.0.0.1");
    snprintf(config.offer.ice_ufrag, sizeof(config.offer.ice_ufrag), "%s",
             CLIENT_UFrag);
    snprintf(config.offer.ice_pwd, sizeof(config.offer.ice_pwd), "%s",
             CLIENT_PWD);
    snprintf(config.offer.fingerprint, sizeof(config.offer.fingerprint), "%s",
             fingerprint);
    snprintf(config.offer.setup, sizeof(config.offer.setup), "actpass");
    snprintf(config.offer.video_mid, sizeof(config.offer.video_mid), "0");
    config.offer.h264_payload_type = RTP_PAYLOAD_TYPE;

    RtcSession *session = NULL;
    char answer[4096];
    size_t answer_length = 0;

    int created = rtc_session_create(&config, &session, answer,
                                     sizeof(answer), &answer_length);

    expect(created == 0 && session != NULL,
           "peer without use_srtp: session created");

    if (created != 0 || session == NULL) {
        SSL_CTX_free(ctx);
        return;
    }

    char server_ufrag[80] = "";
    char server_pwd[128] = "";

    parse_answer_attribute(answer, "ice-ufrag", server_ufrag,
                           sizeof(server_ufrag));
    parse_answer_attribute(answer, "ice-pwd", server_pwd, sizeof(server_pwd));

    int client_fd = socket(AF_INET, SOCK_DGRAM, 0);

    struct sockaddr_in local;

    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(client_fd, (struct sockaddr *) &local, sizeof(local));

    int flags = fcntl(client_fd, F_GETFL, 0);

    if (flags >= 0) {
        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
    }

    struct sockaddr_in session_addr;

    memset(&session_addr, 0, sizeof(session_addr));
    session_addr.sin_family = AF_INET;
    session_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    session_addr.sin_port = htons(config.udp_port);

    char username[192];

    snprintf(username, sizeof(username), "%s:%s", server_ufrag, CLIENT_UFrag);

    uint8_t response[512];
    size_t response_length = 0;

    uint16_t response_type = stun_exchange(session, client_fd, &session_addr,
                                           username, server_pwd, response,
                                           sizeof(response),
                                           &response_length);

    expect(response_type == 0x0101,
           "peer without use_srtp: ICE check still answered");

    DtlsSrtpGlobalStats before;

    dtls_srtp_global_stats(&before);

    SSL *ssl = SSL_new(ctx);
    BIO *bio = BIO_new_dgram(client_fd, BIO_NOCLOSE);

    BIO_ctrl(bio, BIO_CTRL_DGRAM_SET_PEER, 0, &session_addr);
    SSL_set_bio(ssl, bio, bio);
    SSL_set_connect_state(ssl);
    SSL_set_mtu(ssl, 1200);

    int handshake = run_handshake(session, client_fd, ssl);

    expect(handshake == 0,
           "peer without use_srtp: TLS handshake itself completes");

    expect(rtc_session_state(session) != RTC_STREAMING,
           "peer without use_srtp is never moved to streaming");

    DtlsSrtpGlobalStats after;

    dtls_srtp_global_stats(&after);

    expect(after.handshake_failures == before.handshake_failures + 1,
           "the refusal is counted as a handshake failure");
    expect(after.handshakes_completed == before.handshakes_completed,
           "no SRTP key material was exported for it");

    /* The refused session must still be reaped by the watchdog. */
    rtc_session_tick(session, now_ms() + 31000);
    expect(rtc_session_state(session) == RTC_CLOSED,
           "a refused peer is closed by the DTLS watchdog");

    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close(client_fd);
    rtc_session_destroy(session);

    EVP_PKEY_free(client_key);
    X509_free(client_cert);
    client_key = NULL;
    client_cert = NULL;
}

int main(void)
{
    printf("test_rtc_session: starting\n");

    log_init(LOG_LEVEL_ERROR);

    if (dtls_srtp_global_init() != 0) {
        printf("  FAIL dtls_srtp_global_init\n");
        return 1;
    }

    char client_fingerprint[128] = "";

    SSL_CTX *ssl_ctx = create_client_ssl_ctx(client_fingerprint,
                                             sizeof(client_fingerprint), 1);

    if (ssl_ctx == NULL) {
        printf("  FAIL client SSL_CTX\n");
        return 1;
    }

    /* --- 1. session creation -------------------------------------- */

    RtcSessionConfig config;

    memset(&config, 0, sizeof(config));

    config.id = 0x12345678;
    config.udp_port = reserve_free_udp_port();
    config.candidate_count = 1;
    snprintf(config.candidate_ips[0], sizeof(config.candidate_ips[0]),
             "127.0.0.1");
    snprintf(config.offer.ice_ufrag, sizeof(config.offer.ice_ufrag), "%s",
             CLIENT_UFrag);
    snprintf(config.offer.ice_pwd, sizeof(config.offer.ice_pwd), "%s",
             CLIENT_PWD);
    snprintf(config.offer.fingerprint, sizeof(config.offer.fingerprint), "%s",
             client_fingerprint);
    snprintf(config.offer.setup, sizeof(config.offer.setup), "actpass");
    snprintf(config.offer.video_mid, sizeof(config.offer.video_mid), "0");
    config.offer.h264_payload_type = RTP_PAYLOAD_TYPE;

    RtcSession *session = NULL;
    char answer[4096];
    size_t answer_length = 0;

    int created = rtc_session_create(&config, &session, answer,
                                     sizeof(answer), &answer_length);

    expect(created == 0 && session != NULL, "session created");
    expect(rtc_session_state(session) == RTC_NEW, "initial state RTC_NEW");
    expect(strstr(answer, "a=setup:passive") != NULL, "answer is passive");
    expect(strstr(answer, "a=ice-lite") != NULL, "answer is ice-lite");
    expect(strstr(answer, "a=candidate:") != NULL, "answer has a candidate");

    char server_ufrag[80] = "";
    char server_pwd[128] = "";

    expect(parse_answer_attribute(answer, "ice-ufrag", server_ufrag,
                                  sizeof(server_ufrag)) == 0 &&
           parse_answer_attribute(answer, "ice-pwd", server_pwd,
                                  sizeof(server_pwd)) == 0,
           "answer carries ICE credentials");

    /* --- 2. ICE-lite STUN check ----------------------------------- */

    int client_fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (client_fd < 0) {
        printf("  FAIL client socket: %s\n", strerror(errno));
        return 1;
    }

    struct sockaddr_in local;

    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local.sin_port = 0;
    bind(client_fd, (struct sockaddr *) &local, sizeof(local));

    /*
     * Non blocking: a blocking DTLS socket would sit inside
     * SSL_do_handshake() and the test could not pump the session
     * socket while the handshake is in flight.
     */
    int flags = fcntl(client_fd, F_GETFL, 0);

    if (flags >= 0) {
        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
    }

    struct sockaddr_in session_addr;

    memset(&session_addr, 0, sizeof(session_addr));
    session_addr.sin_family = AF_INET;
    session_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    session_addr.sin_port = htons(config.udp_port);

    socklen_t local_length = sizeof(local);

    getsockname(client_fd, (struct sockaddr *) &local, &local_length);

    char username[192];

    snprintf(username, sizeof(username), "%s:%s", server_ufrag, CLIENT_UFrag);

    uint8_t response[512];
    size_t response_length = 0;

    uint16_t response_type = stun_exchange(session, client_fd, &session_addr,
                                           username, server_pwd, response,
                                           sizeof(response),
                                           &response_length);

    expect(response_type == 0x0101, "authenticated check answered 0x0101");
    expect(verify_message_integrity(response, response_length, server_pwd),
           "response MESSAGE-INTEGRITY verifies");

    const uint8_t *mapped = NULL;
    size_t mapped_length = 0;

    expect(find_attr(response, response_length, 0x0020, &mapped,
                     &mapped_length) && mapped_length >= 8,
           "response has XOR-MAPPED-ADDRESS");

    if (mapped != NULL && mapped_length >= 8) {
        uint16_t mapped_port = (uint16_t) (((uint16_t) mapped[2] << 8) |
                                           mapped[3]) ^
                               (uint16_t) (STUN_MAGIC_COOKIE >> 16);
        uint32_t mapped_ip = ((uint32_t) mapped[4] << 24) |
                             ((uint32_t) mapped[5] << 16) |
                             ((uint32_t) mapped[6] << 8) |
                             (uint32_t) mapped[7];

        mapped_ip ^= STUN_MAGIC_COOKIE;

        expect(mapped_port == ntohs(local.sin_port),
               "XOR-MAPPED-ADDRESS port matches the client port");
        expect(mapped_ip == ntohl(local.sin_addr.s_addr),
               "XOR-MAPPED-ADDRESS address matches the client address");
    }

    expect(rtc_session_state(session) == RTC_ICE,
           "ICE validated moves the session to RTC_ICE");

    /* --- 2b. rejection paths -------------------------------------- */

    response_type = stun_exchange(session, client_fd, &session_addr, username,
                                  "wrong-password-0000000000", response,
                                  sizeof(response), &response_length);

    expect(response_type == 0x0111, "bad MESSAGE-INTEGRITY answered 401");

    response_type = stun_exchange(session, client_fd, &session_addr,
                                  "someoneElse:anotherFrag", server_pwd,
                                  response, sizeof(response),
                                  &response_length);

    expect(response_type == 0x0111, "unknown USERNAME answered 401");

    RtcSessionStats stats;

    memset(&stats, 0, sizeof(stats));
    rtc_session_get_stats(session, &stats);

    expect(stats.stun_ok == 1, "exactly one check counted as valid");
    expect(stats.stun_bad >= 2, "rejected checks are counted");

    /* --- 3. DTLS handshake ---------------------------------------- */

    SSL *ssl = SSL_new(ssl_ctx);

    if (ssl == NULL) {
        printf("  FAIL SSL_new\n");
        return 1;
    }

    BIO *bio = BIO_new_dgram(client_fd, BIO_NOCLOSE);

    /*
     * The socket is unconnected, so OpenSSL needs the peer address for
     * every send. Without this the first flight goes to 0.0.0.0:0 and
     * the handshake fails with SSL_ERROR_SYSCALL.
     */
    BIO_ctrl(bio, BIO_CTRL_DGRAM_SET_PEER, 0, &session_addr);

    SSL_set_bio(ssl, bio, bio);
    SSL_set_connect_state(ssl);
    SSL_set_mtu(ssl, 1200);

    expect(run_handshake(session, client_fd, ssl) == 0,
           "DTLS 1.2 handshake completes");
    expect(rtc_session_state(session) == RTC_STREAMING,
           "handshake moves the session to RTC_STREAMING");

    DtlsSrtpGlobalStats dtls_stats;

    dtls_srtp_global_stats(&dtls_stats);

    expect(dtls_stats.handshakes_completed == 1, "handshake counted once");
    expect(dtls_stats.handshake_failures == 0, "no handshake failures");

    /* --- 4/5. SRTP media ------------------------------------------ */

    srtp_t inbound = NULL;
    srtp_t outbound = NULL;

    expect(create_client_srtp(ssl, &inbound, &outbound) == 0,
           "SRTP keys exported on the client side");

    uint8_t access_unit[8192];
    size_t access_unit_length = build_access_unit(access_unit,
                                                  sizeof(access_unit));

    expect(access_unit_length > 0, "test access unit built");

    uint64_t pts_us = 1000000;      /* 1 s, so the RTP timestamp is 90000 */
    uint16_t first_sequence = 0;
    int sent = rtc_session_send_access_unit(session, access_unit,
                                            access_unit_length, pts_us, 1);

    expect(sent > 0, "access unit packetized and sent");

    uint8_t packets[MAX_CAPTURED_PACKETS][MAX_PACKET_SIZE];
    size_t packet_lengths[MAX_CAPTURED_PACKETS];
    int packet_count = 0;
    uint64_t deadline = now_ms() + 2000;

    while (packet_count < (int) (sizeof(packets) / sizeof(packets[0])) &&
           now_ms() < deadline) {
        struct pollfd fds[1];

        fds[0].fd = client_fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;

        if (poll(fds, 1, 50) <= 0) {
            pump_session(session, 0);
            continue;
        }

        ssize_t received = recv(client_fd, packets[packet_count],
                                sizeof(packets[packet_count]), 0);

        if (received <= 0) {
            continue;
        }

        if (((uint8_t) packets[packet_count][1] >= 200) &&
            ((uint8_t) packets[packet_count][1] <= 204)) {
            /* RTCP sender report: not part of the video verification. */
            continue;
        }

        int length = (int) received;

        if (srtp_unprotect(inbound, packets[packet_count], &length) !=
            srtp_err_status_ok) {
            continue;
        }

        packet_lengths[packet_count] = (size_t) length;
        packet_count++;
    }

    expect(packet_count > 0, "SRTP protected RTP received and decrypted");

    if (packet_count > 0) {
        char ssrc_text[32] = "";
        uint32_t expected_ssrc = 0;

        if (parse_answer_attribute(answer, "ssrc", ssrc_text,
                                   sizeof(ssrc_text)) == 0) {
            expected_ssrc = (uint32_t) strtoul(ssrc_text, NULL, 10);
        }

        uint8_t expected_version = (uint8_t) (packets[0][0] >> 6);
        uint8_t payload_type = packets[0][1] & 0x7F;
        first_sequence = (uint16_t) (((uint16_t) packets[0][2] << 8) |
                                              packets[0][3]);
        uint32_t ssrc = ((uint32_t) packets[0][8] << 24) |
                        ((uint32_t) packets[0][9] << 16) |
                        ((uint32_t) packets[0][10] << 8) |
                        (uint32_t) packets[0][11];
        uint32_t timestamp = ((uint32_t) packets[0][4] << 24) |
                             ((uint32_t) packets[0][5] << 16) |
                             ((uint32_t) packets[0][6] << 8) |
                             (uint32_t) packets[0][7];

        expect(expected_version == 2, "RTP version is 2");
        expect(payload_type == RTP_PAYLOAD_TYPE,
               "RTP payload type matches the answer");
        expect(expected_ssrc != 0 && ssrc == expected_ssrc,
               "RTP SSRC matches the answer");
        expect(timestamp == 0,
               "first access unit is the RTP timestamp base");

        int sequence_ok = 1;
        int marker_ok = 1;
        int timestamp_ok = 1;

        for (int i = 0; i < packet_count; i++) {
            uint16_t sequence = (uint16_t) (((uint16_t) packets[i][2] << 8) |
                                            packets[i][3]);
            uint32_t packet_timestamp = ((uint32_t) packets[i][4] << 24) |
                                        ((uint32_t) packets[i][5] << 16) |
                                        ((uint32_t) packets[i][6] << 8) |
                                        (uint32_t) packets[i][7];

            if (sequence != (uint16_t) (first_sequence + i)) {
                sequence_ok = 0;
            }

            if (packet_timestamp != timestamp) {
                timestamp_ok = 0;
            }

            if (i + 1 < packet_count) {
                if (packets[i][1] & 0x80) {
                    marker_ok = 0;
                }
            } else if ((packets[i][1] & 0x80) == 0) {
                marker_ok = 0;
            }
        }

        expect(sequence_ok, "sequence numbers increment without a gap");
        expect(marker_ok, "marker bit set on the last packet of the AU");
        expect(timestamp_ok, "all packets of the AU share one timestamp");

        uint8_t reassembled[8192];
        size_t reassembled_length = reassemble(packets, packet_lengths,
                                               packet_count, reassembled,
                                               sizeof(reassembled));

        expect(reassembled_length == access_unit_length &&
               memcmp(reassembled, access_unit, access_unit_length) == 0,
               "payload reassembles to the original access unit");

        /* --- 6. NACK retransmission ------------------------------- */

        uint8_t nack[64];

        memset(nack, 0, sizeof(nack));
        nack[0] = 0x81;             /* V=2, one feedback packet */
        nack[1] = 205;              /* RTPFB */
        write_be16(nack + 2, 3);    /* length in words minus one */
        write_be32(nack + 4, 0);    /* sender SSRC */
        write_be32(nack + 8, ssrc); /* media SSRC */
        write_be16(nack + 12, first_sequence);
        write_be16(nack + 14, 0);

        int nack_length = 16;

        expect(outbound != NULL &&
               srtp_protect_rtcp(outbound, nack, &nack_length) ==
                   srtp_err_status_ok,
               "NACK protected with SRTCP");

        expect(sendto(client_fd, nack, (size_t) nack_length, 0,
                      (const struct sockaddr *) &session_addr,
                      sizeof(session_addr)) == (ssize_t) nack_length,
               "NACK sent");

        uint16_t retransmit_sequence = 0xFFFF;
        uint64_t nack_deadline = now_ms() + 1000;

        while (now_ms() < nack_deadline && retransmit_sequence == 0xFFFF) {
            pump_session(session, 5);

            struct pollfd fds[1];

            fds[0].fd = client_fd;
            fds[0].events = POLLIN;
            fds[0].revents = 0;

            if (poll(fds, 1, 5) <= 0) {
                continue;
            }

            uint8_t retransmission[MAX_PACKET_SIZE];
            ssize_t received = recv(client_fd, retransmission,
                                    sizeof(retransmission), 0);

            if (received < 12) {
                continue;
            }

            /*
             * The SRTP replay window intentionally rejects a second
             * copy of a sequence number, which is what a correct
             * retransmission looks like, so the packet is checked
             * without unprotecting it: the RTP header is never
             * encrypted, and the datagram is exactly the cached one
             * (plaintext plus the 10 byte AES-CM-SHA1-80 auth tag).
             */
            uint16_t sequence = (uint16_t) (((uint16_t) retransmission[2] << 8) |
                                            retransmission[3]);

            if (sequence == first_sequence &&
                (size_t) received == packet_lengths[0] + 10 &&
                memcmp(retransmission, packets[0], 12) == 0) {
                retransmit_sequence = sequence;
            }
        }

        expect(retransmit_sequence == first_sequence,
               "NACK answered with the original packet");

        memset(&stats, 0, sizeof(stats));
        rtc_session_get_stats(session, &stats);

        expect(stats.nacks_received == 1, "NACK counted");
        expect(stats.retransmissions == 1, "retransmission counted");

    /* --- 6b. the RTP clock advances with capture time ------------ */

    {
        uint64_t second_pts_us = 2500000;   /* +1.5 s on the capture clock */
        int second_sent = rtc_session_send_access_unit(session, access_unit,
                                                       access_unit_length,
                                                       second_pts_us, 0);
        expect(second_sent == sent, "second access unit packetized");

        int second_count = 0;
        uint16_t second_first_sequence = 0;
        uint32_t second_timestamp = 0;
        uint64_t second_deadline = now_ms() + 2000;

        while (second_count < packet_count && now_ms() < second_deadline) {
            struct pollfd fds[1];

            fds[0].fd = client_fd;
            fds[0].events = POLLIN;
            fds[0].revents = 0;

            if (poll(fds, 1, 50) <= 0) {
                pump_session(session, 0);
                continue;
            }

            uint8_t packet[MAX_PACKET_SIZE];
            ssize_t received = recv(client_fd, packet, sizeof(packet), 0);
            int length = (int) received;

            if (length <= 0 ||
                srtp_unprotect(inbound, packet, &length) != srtp_err_status_ok) {
                continue;
            }

            uint16_t sequence = (uint16_t) (((uint16_t) packet[2] << 8) |
                                            packet[3]);
            uint32_t packet_timestamp = ((uint32_t) packet[4] << 24) |
                                        ((uint32_t) packet[5] << 16) |
                                        ((uint32_t) packet[6] << 8) |
                                        (uint32_t) packet[7];

            if (second_count == 0) {
                second_first_sequence = sequence;
                second_timestamp = packet_timestamp;
            }

            second_count++;
        }

        expect(second_count == packet_count,
               "second access unit produces the same packet count");
        expect(second_first_sequence ==
                   (uint16_t) (first_sequence + sent),
               "sequence numbers continue across access units");
        expect(second_timestamp == 135000,
               "RTP timestamp advances 90 kHz per second of capture time");
    }
    }

    /* --- 7. malformed input --------------------------------------- */

    uint8_t garbage[MAX_PACKET_SIZE];

    memset(garbage, 0xA5, sizeof(garbage));

    sendto(client_fd, garbage, 1, 0, (struct sockaddr *) &session_addr,
           sizeof(session_addr));
    sendto(client_fd, garbage, sizeof(garbage), 0,
           (struct sockaddr *) &session_addr, sizeof(session_addr));

    uint8_t malformed_stun[40];

    memset(malformed_stun, 0, sizeof(malformed_stun));
    write_be16(malformed_stun, 0x0001);
    write_be16(malformed_stun + 2, 0x1000);     /* length beyond the datagram */
    write_be32(malformed_stun + 4, STUN_MAGIC_COOKIE);

    sendto(client_fd, malformed_stun, sizeof(malformed_stun), 0,
           (struct sockaddr *) &session_addr, sizeof(session_addr));

    uint8_t indication[20];

    memset(indication, 0, sizeof(indication));
    write_be16(indication, 0x0011);
    write_be16(indication + 2, 0);
    write_be32(indication + 4, STUN_MAGIC_COOKIE);

    sendto(client_fd, indication, sizeof(indication), 0,
           (struct sockaddr *) &session_addr, sizeof(session_addr));

    for (int i = 0; i < 8; i++) {
        pump_session(session, 20);
    }

    expect(rtc_session_state(session) == RTC_STREAMING,
           "session survives malformed datagrams");

    memset(&stats, 0, sizeof(stats));
    rtc_session_get_stats(session, &stats);

    expect(stats.datagrams_rx >= 10, "every datagram is counted");
    expect(stats.stun_bad >= 2, "rejected checks stay counted");

    /* --- 8. idle teardown ----------------------------------------- */

    rtc_session_tick(session, now_ms() + 60000);

    expect(rtc_session_state(session) == RTC_CLOSED,
           "idle timeout closes the session");

    rtc_session_destroy(session);

    SSL_free(ssl);
    SSL_CTX_free(ssl_ctx);

    if (inbound != NULL) {
        srtp_dealloc(inbound);
    }

    if (outbound != NULL) {
        srtp_dealloc(outbound);
    }

    run_peer_without_srtp_scenario();

    srtp_shutdown();
    dtls_srtp_global_shutdown();

    close(client_fd);

    EVP_PKEY_free(client_key);
    X509_free(client_cert);

    printf("test_rtc_session: %s\n", failures == 0 ? "PASS" : "FAIL");

    return failures == 0 ? 0 : 1;
}
