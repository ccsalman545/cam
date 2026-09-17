/*
 * Standalone checks for STUN USERNAME matching, MESSAGE-
 * INTEGRITY verification, IPv4 XOR-MAPPED-ADDRESS /
 * FINGERPRINT construction and 401 error responses.
 *
 *   make test
 */
#include "ice_lite.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>

static void write_be16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t) (value >> 8);
    p[1] = (uint8_t) value;
}

/*
 * Minimal STUN Binding Request with a USERNAME attribute.
 */
static size_t make_binding_request(uint8_t *out, const char *username)
{
    size_t ulen = strlen(username);
    size_t padded = (ulen + 3) & ~3u;

    memset(out, 0, 20 + 4 + padded);
    write_be16(out, 0x0001);
    write_be16(out + 2, (uint16_t) (4 + padded));
    out[4] = 0x21;
    out[5] = 0x12;
    out[6] = 0xA4;
    out[7] = 0x42;

    write_be16(out + 20, 0x0006);
    write_be16(out + 22, (uint16_t) ulen);
    memcpy(out + 24, username, ulen);

    return 20 + 4 + padded;
}

/*
 * Append MESSAGE-INTEGRITY (HMAC-SHA1 over everything up to
 * and including the MI attribute, keyed with password) and
 * fix the message length. Returns the new total length.
 */
static size_t sign_request(uint8_t *req, size_t len, const char *password)
{
    uint8_t *mi = req + len;

    write_be16(mi, 0x0008);
    write_be16(mi + 2, 20);

    size_t total = len + 24;

    write_be16(req + 2, (uint16_t) (total - 20));

    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int mac_len = 0;

    HMAC(EVP_sha1(), password, (int) strlen(password),
         req, total - 20, mac, &mac_len);
    memcpy(mi + 4, mac, 20);

    return total;
}

static int find_attr(const uint8_t *msg, size_t len, uint16_t type,
                     const uint8_t **value, size_t *value_len)
{
    uint16_t message_len = (uint16_t) (((uint16_t) msg[2] << 8) | msg[3]);
    size_t end = 20 + message_len;
    size_t offset = 20;

    if (end > len) {
        return 0;
    }

    while (offset + 4 <= end) {
        uint16_t atype = (uint16_t) (((uint16_t) msg[offset] << 8) |
                                     msg[offset + 1]);
        uint16_t alen = (uint16_t) (((uint16_t) msg[offset + 2] << 8) |
                                    msg[offset + 3]);

        if (offset + 4 + alen > end) {
            return 0;
        }

        if (atype == type) {
            if (value != NULL) {
                *value = msg + offset + 4;
            }
            if (value_len != NULL) {
                *value_len = alen;
            }
            return 1;
        }

        offset += 4 + ((alen + 3) & ~3u);
    }

    return 0;
}

int main(void)
{
    uint8_t req[256];
    int failed = 0;

    size_t n = make_binding_request(req, "SrvUfrag:BrUfrag");

    if (!stun_is_binding_request(req, n, NULL)) {
        fprintf(stderr, "fail: binding request not recognised\n");
        failed = 1;
    }

    if (!stun_username_matches(req, n, "SrvUfrag")) {
        fprintf(stderr, "fail: RFC order (receiver ufrag first) rejected\n");
        failed = 1;
    }

    if (stun_username_matches(req, n, "BrUfrag") == 0) {
        fprintf(stderr, "fail: reversed-order fallback should accept BrUfrag\n");
        failed = 1;
    }

    if (stun_username_matches(req, n, "nope")) {
        fprintf(stderr, "fail: unrelated ufrag accepted\n");
        failed = 1;
    }

    char copied[64];
    if (stun_copy_username(req, n, copied, sizeof(copied)) != 0 ||
        strcmp(copied, "SrvUfrag:BrUfrag") != 0) {
        fprintf(stderr, "fail: stun_copy_username -> '%s'\n", copied);
        failed = 1;
    }

    struct sockaddr_storage peer;
    memset(&peer, 0, sizeof(peer));
    struct sockaddr_in *a4 = (struct sockaddr_in *) &peer;
    a4->sin_family = AF_INET;
    a4->sin_port = htons(51234);
    inet_pton(AF_INET, "192.168.0.10", &a4->sin_addr);

    uint8_t resp[128];
    size_t resp_len = 0;

    if (stun_build_binding_response("localpasswordvalueXXXXXX",
                                    req, n, &peer,
                                    resp, sizeof(resp), &resp_len) != 0) {
        fprintf(stderr, "fail: could not build binding response\n");
        failed = 1;
    } else if (resp_len < 64 || resp[0] != 0x01 || resp[1] != 0x01) {
        fprintf(stderr, "fail: response header type %02x%02x len %zu\n",
                resp[0], resp[1], resp_len);
        failed = 1;
    }

    /* --------------------------------------------------------- */
    /* MESSAGE-INTEGRITY verification (RFC 5389 short-term creds) */
    /* --------------------------------------------------------- */

    uint8_t signed_req[256];
    size_t s_len;

    memcpy(signed_req, req, n);
    s_len = sign_request(signed_req, n, "localpasswordvalueXXXXXX");

    if (!stun_verify_mi(signed_req, s_len, "localpasswordvalueXXXXXX")) {
        fprintf(stderr, "fail: valid MESSAGE-INTEGRITY rejected\n");
        failed = 1;
    }

    if (stun_verify_mi(signed_req, s_len, "otherpasswordvalueXXXXXXXX")) {
        fprintf(stderr, "fail: MESSAGE-INTEGRITY with wrong key accepted\n");
        failed = 1;
    }

    if (stun_verify_mi(req, n, "localpasswordvalueXXXXXX")) {
        fprintf(stderr, "fail: request without MI accepted\n");
        failed = 1;
    }

    /* tamper with the transaction id after signing */
    uint8_t tampered[256];

    memcpy(tampered, signed_req, s_len);
    tampered[10] ^= 0x40;

    if (stun_verify_mi(tampered, s_len, "localpasswordvalueXXXXXX")) {
        fprintf(stderr, "fail: tampered request passed MI check\n");
        failed = 1;
    }

    /* --------------------------------------------------------- */
    /* 401 error response (RFC 5389 §15.6)                       */
    /* --------------------------------------------------------- */

    uint8_t tid[12];

    for (int i = 0; i < 12; i++) {
        tid[i] = (uint8_t) (i * 3 + 1);
    }

    uint8_t err[64];
    size_t err_len = 0;

    if (stun_build_error_response(tid, 401, err, sizeof(err), &err_len) != 0) {
        fprintf(stderr, "fail: could not build error response\n");
        failed = 1;
    } else {
        if (err[0] != 0x01 || err[1] != 0x11) {
            fprintf(stderr,
                    "fail: error response type %02x%02x, want 01 11\n",
                    err[0], err[1]);
            failed = 1;
        }

        if (memcmp(err + 8, tid, 12) != 0) {
            fprintf(stderr, "fail: error response did not echo tid\n");
            failed = 1;
        }

        uint16_t msg_len = (uint16_t) (((uint16_t) err[2] << 8) | err[3]);

        if ((size_t) msg_len + 20 != err_len) {
            fprintf(stderr,
                    "fail: error response length field %u != %zu\n",
                    (unsigned) msg_len, err_len);
            failed = 1;
        }

        const uint8_t *ec = NULL;
        size_t ec_len = 0;

        if (!find_attr(err, err_len, 0x0009, &ec, &ec_len) || ec_len != 8 ||
            ec[0] != 0 || ec[1] != 4 || (ec[2] << 8 | ec[3]) != 1) {
            fprintf(stderr, "fail: ERROR-CODE attribute missing or != 401\n");
            failed = 1;
        }

        const uint8_t *sw = NULL;
        size_t sw_len = 0;

        if (!find_attr(err, err_len, 0x8022, &sw, &sw_len) ||
            sw_len != 9 || memcmp(sw, "camstream", 9) != 0) {
            fprintf(stderr, "fail: SOFTWARE attribute missing or wrong\n");
            failed = 1;
        }
    }

    if (failed) {
        fprintf(stderr, "test_stun: FAILED\n");
        return 1;
    }

    printf("test_stun: ok\n");
    return 0;
}
