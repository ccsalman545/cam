#define _POSIX_C_SOURCE 200809L

/*
 * dtls_srtp.c
 *
 * DTLS server + SRTP protection engine built on OpenSSL and
 * libsrtp2. See dtls_srtp.h for the integration contract.
 */
#include "dtls_srtp.h"

#include "log.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <srtp2/srtp.h>

#define DTLS_RX_QUEUE 64
#define DTLS_MTU 1200
#define SRTP_KEY_MATERIAL_LEN 60     /* 2 x (16 key + 14 salt) */

static SSL_CTX *g_ctx;
static X509 *g_cert;
static EVP_PKEY *g_key;
static char g_fingerprint[128];
static int g_srtp_initialized;
static DtlsSrtpGlobalStats g_stats;

struct DtlsRxPacket {
    uint8_t *data;
    size_t len;
};

struct DtlsSrtp {
    SSL *ssl;
    BIO *rbio;
    BIO *wbio;

    DtlsSrtpCallbacks cb;
    DtlsSrtpState state;

    struct DtlsRxPacket rxq[DTLS_RX_QUEUE];
    size_t rx_head;         /* next to read */
    size_t rx_tail;         /* next to write */
    size_t rx_count;

    /*
     * libsrtp2 sessions. srtp_t is an opaque pointer typedef in
     * libsrtp2 (typedef srtp_ctx_t *srtp_t), so a bare srtp_t is
     * the session handle itself.
     */
    srtp_t srtp_out;
    srtp_t srtp_in;
    int srtp_ready;

    char expected_fingerprint[128];
    char failure_reason[96];
    int handshake_started;
    int fingerprint_ok;

    uint8_t read_buffer[4096];
};

/*
 * OpenSSL keeps errors on a per-thread queue; drain it so the log
 * line names the failing call and the library reason.
 */
static void dtls_log_openssl(const char *operation)
{
    unsigned long code = ERR_get_error();
    char text[128];

    if (code == 0) {
        log_error("dtls", "%s failed (no OpenSSL error queued)", operation);
        return;
    }

    ERR_error_string_n(code, text, sizeof(text));
    log_error("dtls", "%s failed: %s", operation, text);
}

/* ------------------------------------------------------------------ */
/* Global certificate and SSL_CTX                                      */
/* ------------------------------------------------------------------ */

static int dtls_verify_allow_self_signed(int preverify_ok,
                                         X509_STORE_CTX *ctx)
{
    (void) preverify_ok;
    (void) ctx;

    /*
     * WebRTC certificates are self-signed. The SDP fingerprint
     * is compared after the handshake in derive_srtp_keys().
     */
    return 1;
}

static int generate_certificate(void)
{
    g_key = EVP_EC_gen("P-256");
    if (g_key == NULL) {
        return -1;
    }

    g_cert = X509_new();
    if (g_cert == NULL) {
        EVP_PKEY_free(g_key);
        g_key = NULL;
        return -1;
    }

    X509_set_version(g_cert, 2);

    ASN1_INTEGER_set(X509_get_serialNumber(g_cert),
                     (long) time(NULL));

    X509_gmtime_adj(X509_getm_notBefore(g_cert), -3600);
    X509_gmtime_adj(X509_getm_notAfter(g_cert), 365L * 24 * 3600);

    X509_set_pubkey(g_cert, g_key);

    X509_NAME *name = X509_get_subject_name(g_cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               (const unsigned char *) "camstream",
                               -1, -1, 0);
    X509_set_issuer_name(g_cert, name);

    if (X509_sign(g_cert, g_key, EVP_sha256()) == 0) {
        return -1;
    }

    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int md_len = 0;

    if (X509_digest(g_cert, EVP_sha256(), md, &md_len) != 1) {
        return -1;
    }

    /*
     * RFC 7999: the attribute value is "<hash algo> <hex with
     * colons>". Browsers verify the peer certificate against
     * this value, so the "sha-256 " prefix is mandatory.
     */
    size_t offset = (size_t) snprintf(g_fingerprint, sizeof(g_fingerprint),
                                      "sha-256 ");

    for (unsigned int i = 0; i < md_len; i++) {
        offset += (size_t) snprintf(g_fingerprint + offset,
                                    sizeof(g_fingerprint) - offset,
                                    "%s%02X", i ? ":" : "", md[i]);
    }

    return 0;
}

int dtls_srtp_global_init(void)
{
    if (g_ctx != NULL) {
        return 0;
    }

    if (!g_srtp_initialized) {
        srtp_err_status_t srtp_status = srtp_init();

        if (srtp_status != srtp_err_status_ok) {
            log_error("dtls", "srtp_init() failed: status=%d", (int) srtp_status);
            return -1;
        }
        g_srtp_initialized = 1;
    }

    if (generate_certificate() != 0) {
        log_error("dtls", "self signed certificate generation failed");
        return -1;
    }

    g_ctx = SSL_CTX_new(DTLS_method());
    if (g_ctx == NULL) {
        dtls_log_openssl("SSL_CTX_new(DTLS_method)");
        return -1;
    }

    SSL_CTX_use_certificate(g_ctx, g_cert);
    SSL_CTX_use_PrivateKey(g_ctx, g_key);

    if (SSL_CTX_check_private_key(g_ctx) != 1) {
        dtls_log_openssl("SSL_CTX_check_private_key");
        return -1;
    }

    /*
     * SRTP protection profile. AES128 CM with HMAC-SHA1-80 is
     * the universally implemented WebRTC baseline profile and
     * keeps the exported key layout at exactly 60 bytes.
     */
    if (SSL_CTX_set_tlsext_use_srtp(g_ctx, "SRTP_AES128_CM_SHA1_80") != 0) {
        dtls_log_openssl("SSL_CTX_set_tlsext_use_srtp");
        return -1;
    }

    /*
     * Our custom BIO cannot answer MTU queries, so OpenSSL
     * must use the fixed value set per session.
     */
    SSL_CTX_set_options(g_ctx, SSL_OP_NO_QUERY_MTU |
                               SSL_OP_NO_RENEGOTIATION);

    SSL_CTX_set_timeout(g_ctx, 30);

    /*
     * WebRTC DTLS certificates are self-signed. The default
     * OpenSSL verify callback rejects them against the system
     * trust store, which aborts the handshake before we can
     * check the SDP fingerprint. Accept any peer cert here;
     * derive_srtp_keys() compares SHA-256 against a=fingerprint.
     */
    SSL_CTX_set_verify(g_ctx,
                       SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                       dtls_verify_allow_self_signed);

#ifdef DTLS1_2_VERSION
    SSL_CTX_set_min_proto_version(g_ctx, DTLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(g_ctx, DTLS1_2_VERSION);
#endif

    log_info("dtls", "local certificate fingerprint %s", g_fingerprint);

    return 0;
}

void dtls_srtp_global_shutdown(void)
{
    if (g_ctx != NULL) {
        SSL_CTX_free(g_ctx);
        g_ctx = NULL;
    }

    if (g_cert != NULL) {
        X509_free(g_cert);
        g_cert = NULL;
    }

    if (g_key != NULL) {
        EVP_PKEY_free(g_key);
        g_key = NULL;
    }
}

const char *dtls_srtp_local_fingerprint(void)
{
    return g_fingerprint;
}

/* ------------------------------------------------------------------ */
/* Custom BIO: rx queue + tx callback                                  */
/* ------------------------------------------------------------------ */

static int bio_rx_write(BIO *b, const char *buf, int len)
{
    (void) b; (void) buf; (void) len;
    return -1;
}

static int bio_rx_read(BIO *b, char *buf, int size)
{
    DtlsSrtp *session = BIO_get_data(b);

    if (session->rx_count == 0) {
        BIO_set_retry_read(b);
        return -1;
    }

    struct DtlsRxPacket *pkt = &session->rxq[session->rx_head];

    if ((size_t) size < pkt->len) {
        /*
         * A single datagram must fit into one read, which is
         * guaranteed by our 4 KB read buffer.
         */
        BIO_set_retry_read(b);
        session->rx_count--;
        session->rx_head = (session->rx_head + 1) % DTLS_RX_QUEUE;
        free(pkt->data);
        pkt->data = NULL;
        return -1;
    }

    memcpy(buf, pkt->data, pkt->len);

    free(pkt->data);
    pkt->data = NULL;

    session->rx_count--;
    session->rx_head = (session->rx_head + 1) % DTLS_RX_QUEUE;

    return (int) pkt->len;
}

static long bio_rx_ctrl(BIO *b, int cmd, long num, void *ptr)
{
    DtlsSrtp *session = BIO_get_data(b);

    switch (cmd) {
    case BIO_CTRL_PENDING:
        return session->rx_count > 0 ? 1 : 0;
    case BIO_CTRL_FLUSH:
        return 1;
    default:
        (void) num; (void) ptr;
        return 0;
    }
}

static int bio_create(BIO *b)
{
    BIO_set_shutdown(b, 0);
    BIO_set_init(b, 1);
    return 1;
}

static int bio_destroy(BIO *b)
{
    (void) b;
    return 1;
}

static int bio_tx_write(BIO *b, const char *buf, int len)
{
    DtlsSrtp *session = BIO_get_data(b);

    if (session->cb.send_udp == NULL || len <= 0) {
        return -1;
    }

    session->cb.send_udp(session->cb.user,
                         (const uint8_t *) buf,
                         (size_t) len);

    return len;
}

static int bio_tx_read(BIO *b, char *buf, int size)
{
    (void) b; (void) buf; (void) size;
    return -1;
}

static long bio_tx_ctrl(BIO *b, int cmd, long num, void *ptr)
{
    (void) b;

    switch (cmd) {
    case BIO_CTRL_FLUSH:
        return 1;
    case BIO_CTRL_DGRAM_QUERY_MTU:
        return DTLS_MTU;
    case BIO_CTRL_PENDING:
        return 0;
    default:
        (void) num; (void) ptr;
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Session                                                             */
/* ------------------------------------------------------------------ */

static BIO_METHOD *g_rx_method;
static BIO_METHOD *g_tx_method;

static void dtls_set_state(DtlsSrtp *session, DtlsSrtpState state)
{
    if (session->state == state) {
        return;
    }

    session->state = state;

    if (session->cb.on_state != NULL) {
        session->cb.on_state(session->cb.user, state);
    }
}

static int fingerprint_hex_equal(const char *sdp_value, unsigned char *md,
                                 unsigned int md_len)
{
    char normalized[128];
    size_t out = 0;

    /*
     * The SDP value is "<hash algo> <hex with colons>", e.g.
     * "sha-256 AA:BB:..". Skip the algorithm token first, then
     * strip colons and fold case.
     */
    const char *p = sdp_value;

    if (*p != 0) {
        const char *space = strchr(p, ' ');

        if (space != NULL) {
            p = space + 1;
        }
    }

    for (; *p != 0 && out + 1 < sizeof(normalized); p++) {
        if (*p == ':') {
            continue;
        }
        normalized[out++] = (char) ((*p >= 'a' && *p <= 'f') ? *p - 32 : *p);
    }
    normalized[out] = 0;

    char actual[104];
    size_t actual_out = 0;

    for (unsigned int i = 0; i < md_len; i++) {
        actual_out += (size_t) snprintf(actual + actual_out,
                                        sizeof(actual) - actual_out,
                                        "%02X", md[i]);
    }
    actual[actual_out] = 0;

    return strcmp(normalized, actual) == 0;
}

static int derive_srtp_keys(DtlsSrtp *session)
{
    unsigned char material[SRTP_KEY_MATERIAL_LEN];
    uint8_t client_key[30];
    uint8_t server_key[30];

    /*
     * Export order (RFC 5764):
     *   client write key (16) | server write key (16)
     *   client write salt (14) | server write salt (14)
     */
    if (SSL_export_keying_material(session->ssl,
                                   material,
                                   sizeof(material),
                                   "EXTRACTOR-dtls_srtp",
                                   19,
                                   NULL, 0, 0) != 1) {
        dtls_log_openssl("SSL_export_keying_material");
        return -1;
    }

    memcpy(client_key, material, 16);
    memcpy(client_key + 16, material + 32, 14);

    memcpy(server_key, material + 16, 16);
    memcpy(server_key + 16, material + 46, 14);

    srtp_policy_t policy;

    memset(&policy, 0, sizeof(policy));

    /*
     * The profile selected during the handshake decides the
     * crypto policies. Both offered profiles use the same key
     * sizes, so use the default AES128 CM policies.
     */
    srtp_crypto_policy_set_rtp_default(&policy.rtp);
    srtp_crypto_policy_set_rtcp_default(&policy.rtcp);

    policy.ssrc.type = ssrc_any_outbound;
    policy.key = server_key;
    policy.allow_repeat_tx = 1;

    srtp_err_status_t status = srtp_create(&session->srtp_out, &policy);

    if (status != srtp_err_status_ok) {
        log_error("dtls", "srtp_create(outbound) failed: status=%d",
                  (int) status);
        return -1;
    }

    policy.ssrc.type = ssrc_any_inbound;
    policy.key = client_key;

    status = srtp_create(&session->srtp_in, &policy);

    if (status != srtp_err_status_ok) {
        log_error("dtls", "srtp_create(inbound) failed: status=%d",
                  (int) status);
        return -1;
    }

    /*
     * Verify the certificate fingerprint from signaling.
     */
    if (session->expected_fingerprint[0] != 0) {
        X509 *peer = SSL_get1_peer_certificate(session->ssl);

        if (peer == NULL) {
            snprintf(session->failure_reason, sizeof(session->failure_reason),
                     "peer completed the handshake without a certificate");
            g_stats.handshake_failures++;
            log_error("dtls", "handshake failed: %s", session->failure_reason);
            return -1;
        }

        unsigned char md[EVP_MAX_MD_SIZE];
        unsigned int md_len = 0;

        int ok = X509_digest(peer, EVP_sha256(), md, &md_len) == 1 &&
                 fingerprint_hex_equal(session->expected_fingerprint,
                                       md, md_len);

        X509_free(peer);

        if (!ok) {
            g_stats.fingerprint_mismatches++;
            g_stats.handshake_failures++;
            snprintf(session->failure_reason, sizeof(session->failure_reason),
                     "peer certificate does not match the a=fingerprint in "
                     "the offer");
            return -1;
        }
    }

    session->fingerprint_ok = 1;
    session->srtp_ready = 1;

    return 0;
}

static void pump_ssl(DtlsSrtp *session)
{
    if (session->state == DTLS_SRTP_CLOSED ||
        session->state == DTLS_SRTP_FAILED) {
        return;
    }

    if (!session->handshake_started) {
        session->handshake_started = 1;
        g_stats.handshakes_started++;
        dtls_set_state(session, DTLS_SRTP_HANDSHAKING);

        int rc = SSL_accept(session->ssl);

        if (rc == 1) {
            /* Unusual but possible: single flight handshake. */
        } else {
            int err = SSL_get_error(session->ssl, rc);

            if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
                snprintf(session->failure_reason,
                         sizeof(session->failure_reason),
                         "SSL_accept: %s (%d)",
                         err == SSL_ERROR_SSL ? "protocol error" :
                         err == SSL_ERROR_SYSCALL ? "unexpected EOF" :
                         "handshake rejected", err);
                g_stats.handshake_failures++;
                log_error("dtls", "handshake failed: %s", session->failure_reason);
                dtls_log_openssl("SSL_accept");
                dtls_set_state(session, DTLS_SRTP_FAILED);
                return;
            }
        }
    } else {
        int rc = SSL_read(session->ssl,
                          session->read_buffer,
                          (int) sizeof(session->read_buffer));

        if (rc < 0) {
            int err = SSL_get_error(session->ssl, rc);

            if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
                snprintf(session->failure_reason,
                         sizeof(session->failure_reason),
                         "SSL_read: error %d", err);
                dtls_log_openssl("SSL_read");
                dtls_set_state(session, DTLS_SRTP_FAILED);
                return;
            }
        } else if (rc == 0) {
            /*
             * close_notify from the peer.
             */
            dtls_set_state(session, DTLS_SRTP_CLOSED);
            return;
        }
    }

    if (!session->srtp_ready && SSL_is_init_finished(session->ssl) == 1) {
        if (derive_srtp_keys(session) != 0) {
            if (session->failure_reason[0] == 0) {
                snprintf(session->failure_reason,
                         sizeof(session->failure_reason),
                         "SRTP key derivation failed");
            }
            g_stats.handshake_failures++;
            log_error("dtls", "handshake failed: %s", session->failure_reason);
            dtls_set_state(session, DTLS_SRTP_FAILED);
            return;
        }

        session->failure_reason[0] = 0;
        g_stats.handshakes_completed++;

        dtls_set_state(session, DTLS_SRTP_CONNECTED);

        if (session->cb.on_connected != NULL) {
            session->cb.on_connected(session->cb.user);
        }
    }
}

DtlsSrtp *dtls_srtp_session_create(const DtlsSrtpCallbacks *callbacks)
{
    if (g_ctx == NULL) {
        return NULL;
    }

    DtlsSrtp *session = calloc(1, sizeof(*session));
    if (session == NULL) {
        return NULL;
    }

    session->cb = *callbacks;
    session->state = DTLS_SRTP_INIT;

    if (g_rx_method == NULL) {
        g_rx_method = BIO_meth_new(BIO_TYPE_SOURCE_SINK, "camstream rx");
        BIO_meth_set_write(g_rx_method, bio_rx_write);
        BIO_meth_set_read(g_rx_method, bio_rx_read);
        BIO_meth_set_ctrl(g_rx_method, bio_rx_ctrl);
        BIO_meth_set_create(g_rx_method, bio_create);
        BIO_meth_set_destroy(g_rx_method, bio_destroy);
    }

    if (g_tx_method == NULL) {
        g_tx_method = BIO_meth_new(BIO_TYPE_SOURCE_SINK, "camstream tx");
        BIO_meth_set_write(g_tx_method, bio_tx_write);
        BIO_meth_set_read(g_tx_method, bio_tx_read);
        BIO_meth_set_ctrl(g_tx_method, bio_tx_ctrl);
        BIO_meth_set_create(g_tx_method, bio_create);
        BIO_meth_set_destroy(g_tx_method, bio_destroy);
    }

    session->rbio = BIO_new(g_rx_method);
    session->wbio = BIO_new(g_tx_method);

    if (session->rbio == NULL || session->wbio == NULL) {
        BIO_free(session->rbio);
        BIO_free(session->wbio);
        free(session);
        return NULL;
    }

    BIO_set_data(session->rbio, session);
    BIO_set_data(session->wbio, session);

    session->ssl = SSL_new(g_ctx);

    if (session->ssl == NULL) {
        BIO_free(session->rbio);
        BIO_free(session->wbio);
        free(session);
        return NULL;
    }

    SSL_set_bio(session->ssl, session->rbio, session->wbio);
    SSL_set_accept_state(session->ssl);

    /*
     * Fixed MTU because NO_QUERY_MTU is set and the BIO
     * cannot be queried.
     */
    SSL_set_mtu(session->ssl, DTLS_MTU);

    return session;
}

void dtls_srtp_set_expected_fingerprint(DtlsSrtp *session,
                                        const char *fingerprint)
{
    if (session == NULL || fingerprint == NULL) {
        return;
    }

    snprintf(session->expected_fingerprint,
             sizeof(session->expected_fingerprint),
             "%s", fingerprint);
}

void dtls_srtp_on_udp(DtlsSrtp *session,
                      const uint8_t *packet,
                      size_t len)
{
    if (session == NULL || session->state == DTLS_SRTP_FAILED ||
        session->state == DTLS_SRTP_CLOSED) {
        return;
    }

    /*
     * Drop ChangeCipherSpec and Alerts from unverified peers
     * only after handshake started, otherwise queue them.
     */
    if (session->rx_count >= DTLS_RX_QUEUE) {
        /* Queue overflow, drop oldest. */
        free(session->rxq[session->rx_head].data);
        session->rxq[session->rx_head].data = NULL;
        session->rx_head = (session->rx_head + 1) % DTLS_RX_QUEUE;
        session->rx_count--;
    }

    size_t tail = session->rx_tail;

    session->rxq[tail].data = malloc(len);

    if (session->rxq[tail].data == NULL) {
        return;
    }

    memcpy(session->rxq[tail].data, packet, len);
    session->rxq[tail].len = len;

    session->rx_tail = (tail + 1) % DTLS_RX_QUEUE;
    session->rx_count++;

    pump_ssl(session);
}

void dtls_srtp_tick(DtlsSrtp *session)
{
    if (session == NULL || session->ssl == NULL) {
        return;
    }

    /*
     * Do not SSL_accept() from INIT: that used to start the
     * handshake (and emit DTLS records) before any ClientHello
     * and before ICE had a peer address, so the first flight
     * was dropped.
     */
    if (session->state != DTLS_SRTP_HANDSHAKING) {
        return;
    }

    struct timeval tv;

    if (DTLSv1_get_timeout(session->ssl, &tv) == 1) {
        if (tv.tv_sec == 0 && tv.tv_usec == 0) {
            DTLSv1_handle_timeout(session->ssl);
        }
    }

    pump_ssl(session);
}

int dtls_srtp_next_timeout_ms(const DtlsSrtp *session)
{
    if (session == NULL || session->ssl == NULL) {
        return -1;
    }

    if (session->state != DTLS_SRTP_HANDSHAKING &&
        session->state != DTLS_SRTP_INIT) {
        return -1;
    }

    struct timeval tv;

    if (DTLSv1_get_timeout(session->ssl, &tv) != 1) {
        return -1;
    }

    return (int) (tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

int dtls_srtp_send_rtp(DtlsSrtp *session, uint8_t *pkt, size_t *len)
{
    if (session == NULL || !session->srtp_ready) {
        return -1;
    }

    int length = (int) *len;

    if (srtp_protect(session->srtp_out, pkt, &length) != srtp_err_status_ok) {
        return -1;
    }

    *len = (size_t) length;

    session->cb.send_udp(session->cb.user, pkt, *len);

    return 0;
}

int dtls_srtp_send_rtcp(DtlsSrtp *session, uint8_t *pkt, size_t *len)
{
    if (session == NULL || !session->srtp_ready) {
        return -1;
    }

    int length = (int) *len;

    if (srtp_protect_rtcp(session->srtp_out, pkt, &length) != srtp_err_status_ok) {
        return -1;
    }

    *len = (size_t) length;

    session->cb.send_udp(session->cb.user, pkt, *len);

    return 0;
}

int dtls_srtp_unprotect_rtcp(DtlsSrtp *session, uint8_t *pkt, size_t *len)
{
    if (session == NULL || !session->srtp_ready) {
        return -1;
    }

    int length = (int) *len;

    if (srtp_unprotect_rtcp(session->srtp_in, pkt, &length) != srtp_err_status_ok) {
        return -1;
    }

    *len = (size_t) length;

    return 0;
}

void dtls_srtp_close(DtlsSrtp *session)
{
    if (session == NULL) {
        return;
    }

    if (session->ssl != NULL && session->state == DTLS_SRTP_CONNECTED) {
        SSL_shutdown(session->ssl);
    }

    dtls_set_state(session, DTLS_SRTP_CLOSED);
}

void dtls_srtp_session_destroy(DtlsSrtp *session)
{
    if (session == NULL) {
        return;
    }

    if (session->ssl != NULL) {
        SSL_free(session->ssl);     /* frees the BIOs as well */
    }

    if (session->srtp_out != NULL) {
        srtp_dealloc(session->srtp_out);
    }

    if (session->srtp_in != NULL) {
        srtp_dealloc(session->srtp_in);
    }

    for (size_t i = 0; i < DTLS_RX_QUEUE; i++) {
        free(session->rxq[i].data);
    }

    free(session);
}

void dtls_srtp_global_stats(DtlsSrtpGlobalStats *out)
{
    if (out != NULL) {
        *out = g_stats;
    }
}

const char *dtls_srtp_failure_reason(const DtlsSrtp *session)
{
    return session != NULL ? session->failure_reason : "";
}
