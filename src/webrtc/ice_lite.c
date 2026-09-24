/*
 * ice_lite.c
 *
 * STUN and UDP helpers, see ice_lite.h.
 */
#include "ice_lite.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

static uint32_t g_crc32_table[256];
static void init_crc32_table(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320UL : crc >> 1;
        }
        g_crc32_table[i] = crc;
    }
}
static pthread_once_t g_crc32_once = PTHREAD_ONCE_INIT;

RtcPacketClass rtc_classify_packet(const uint8_t *buf, size_t len)
{
    if (len < 1) {
        return RTC_PKT_OTHER;
    }

    if (buf[0] <= 3) {
        return RTC_PKT_STUN;
    }

    if (buf[0] >= 20 && buf[0] <= 63) {
        return RTC_PKT_DTLS;
    }

    /*
     * First byte >= 128 covers both plain RTP (V=2, P=0, X=0:
     * 128 to 191) and SRTCP (V=2 plus padding/RR bits: 192 to
     * 255). Limiting the range to 128 to 191 dropped every
     * inbound SRTCP packet, so browser NACK/PLI/FIR/BYE never
     * reached the RTCP handler.
     */
    if (buf[0] >= 128) {
        return RTC_PKT_RTP;
    }

    return RTC_PKT_OTHER;
}

int udp_socket_create(uint16_t requested_port, uint16_t *actual_port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (fd < 0) {
        return -1;
    }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in address;

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(requested_port);

    if (bind(fd, (struct sockaddr *) &address, sizeof(address)) != 0) {
        close(fd);
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    if (actual_port != NULL) {
        struct sockaddr_in bound;
        socklen_t bound_len = sizeof(bound);

        if (getsockname(fd, (struct sockaddr *) &bound, &bound_len) == 0) {
            *actual_port = ntohs(bound.sin_port);
        } else {
            close(fd);
            return -1;
        }
    }

    return fd;
}

static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t) ((uint16_t) p[0] << 8 | p[1]);
}

static void write_be16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t) (value >> 8);
    p[1] = (uint8_t) value;
}

static uint32_t crc32_stun(const uint8_t *data, size_t length)
{
    pthread_once(&g_crc32_once, init_crc32_table);

    uint32_t crc = 0xFFFFFFFFUL;

    for (size_t i = 0; i < length; i++) {
        crc = g_crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }

    return crc ^ 0xFFFFFFFFUL;
}

/*
 * Walk attributes and optionally return one attribute by type.
 */
static int stun_find_attribute(const uint8_t *buf,
                               size_t len,
                               uint16_t wanted,
                               const uint8_t **value,
                               size_t *value_len)
{
    if (len < STUN_HEADER_SIZE) {
        return 0;
    }

    uint16_t message_len = read_be16(buf + 2);

    if ((size_t) message_len + STUN_HEADER_SIZE > len) {
        return 0;
    }

    size_t offset = STUN_HEADER_SIZE;
    size_t end = STUN_HEADER_SIZE + message_len;

    while (offset + 4 <= end) {
        uint16_t type = read_be16(buf + offset);
        uint16_t attr_len = read_be16(buf + offset + 2);

        if (offset + 4 + attr_len > end) {
            return 0;
        }

        if (type == wanted) {
            if (value != NULL) {
                *value = buf + offset + 4;
            }
            if (value_len != NULL) {
                *value_len = attr_len;
            }
            return 1;
        }

        offset += 4 + ((attr_len + 3) & ~3u);
    }

    return 0;
}

int stun_is_binding_request(const uint8_t *buf, size_t len,
                            uint8_t tid[12])
{
    if (len < STUN_HEADER_SIZE) {
        return 0;
    }

    uint16_t type = read_be16(buf);
    uint16_t message_len = read_be16(buf + 2);
    uint32_t cookie = ((uint32_t) buf[4] << 24) |
                      ((uint32_t) buf[5] << 16) |
                      ((uint32_t) buf[6] << 8) |
                      (uint32_t) buf[7];

    if (type != 0x0001) {
        return 0;
    }

    if (cookie != STUN_MAGIC_COOKIE) {
        return 0;
    }

    if ((size_t) message_len + STUN_HEADER_SIZE > len) {
        return 0;
    }

    if (tid != NULL) {
        memcpy(tid, buf + 8, 12);
    }

    return 1;
}

int stun_is_binding_indication(const uint8_t *buf, size_t len)
{
    if (len < STUN_HEADER_SIZE) {
        return 0;
    }

    uint16_t type = read_be16(buf);
    uint16_t message_len = read_be16(buf + 2);
    uint32_t cookie = ((uint32_t) buf[4] << 24) |
                      ((uint32_t) buf[5] << 16) |
                      ((uint32_t) buf[6] << 8) |
                      (uint32_t) buf[7];

    return type == 0x0011 && cookie == STUN_MAGIC_COOKIE &&
           (size_t) message_len + STUN_HEADER_SIZE <= len;
}

int stun_copy_username(const uint8_t *buf, size_t len,
                       char *out, size_t out_size)
{
    const uint8_t *username = NULL;
    size_t username_len = 0;

    if (out == NULL || out_size == 0) {
        return -1;
    }

    out[0] = 0;

    if (!stun_find_attribute(buf, len, 0x0006, &username, &username_len)) {
        return -1;
    }

    if (username_len >= out_size) {
        username_len = out_size - 1;
    }

    memcpy(out, username, username_len);
    out[username_len] = 0;

    return 0;
}

int stun_username_matches(const uint8_t *buf, size_t len,
                          const char *local_ufrag)
{
    const uint8_t *username = NULL;
    size_t username_len = 0;

    if (local_ufrag == NULL || local_ufrag[0] == 0) {
        return 0;
    }

    if (!stun_find_attribute(buf, len, 0x0006, &username, &username_len)) {
        return 0;
    }

    size_t ufrag_len = strlen(local_ufrag);

    /*
     * RFC 8445 §7.3.1.1: USERNAME is
     * "<controlling-ufrag>:<controlled-ufrag>". A browser is
     * the controlling agent (offerer), so its checks arrive
     * as "<browser-ufrag>:<server-ufrag>" — our ufrag is the
     * SECOND component. Some stacks send the reverse. Either
     * order is accepted; what matters is that our ufrag
     * appears exactly once, which is what short-term
     * credentials bind the check to.
     */
    if (username_len > ufrag_len &&
        username[ufrag_len] == ':' &&
        memcmp(username, local_ufrag, ufrag_len) == 0) {
        return 1;
    }

    /*
     * Defensive fallback: accept the reversed ordering so a
     * peer that swapped the fragments still completes ICE.
     */
    const uint8_t *colon = memchr(username, ':', username_len);

    if (colon != NULL) {
        const uint8_t *second = colon + 1;
        size_t second_len = (size_t) ((username + username_len) - second);

        if (second_len == ufrag_len &&
            memcmp(second, local_ufrag, ufrag_len) == 0) {
            return 1;
        }
    }

    return 0;
}

int stun_build_binding_response(const char *local_pwd,
                                const uint8_t *request,
                                size_t request_len,
                                const struct sockaddr_storage *peer,
                                uint8_t *out,
                                size_t out_capacity,
                                size_t *out_len)
{
    /*
     * 20 header + 12 XOR-MAPPED-ADDRESS + 24 MESSAGE-INTEGRITY +
     * 8 FINGERPRINT = 64 bytes. IPv4 only: the media socket is
     * AF_INET, so an IPv6 peer cannot appear here.
     */
    if (request_len < STUN_HEADER_SIZE || out_capacity < 64 ||
        peer == NULL || peer->ss_family != AF_INET || local_pwd == NULL) {
        return -1;
    }

    /*
     * Response header: type 0x0101, same transaction id.
     */
    out[0] = 0x01;
    out[1] = 0x01;
    memcpy(out + 4, request + 4, 16);       /* cookie + tid */

    size_t payload = 0;

    /*
     * XOR-MAPPED-ADDRESS.
     */
    {
        uint8_t *attr = out + STUN_HEADER_SIZE;

        const struct sockaddr_in *a4 = (const struct sockaddr_in *) peer;

        write_be16(attr, 0x0020);
        write_be16(attr + 2, 8);
        attr[4] = 0;
        attr[5] = 0x01;                 /* IPv4 */

        /* RFC 5389 section 15.2: port and address XOR the magic cookie. */
        write_be16(attr + 6, (uint16_t) (ntohs(a4->sin_port) ^ 0x2112));

        uint32_t addr = ntohl(a4->sin_addr.s_addr) ^ STUN_MAGIC_COOKIE;

        attr[8] = (uint8_t) (addr >> 24);
        attr[9] = (uint8_t) (addr >> 16);
        attr[10] = (uint8_t) (addr >> 8);
        attr[11] = (uint8_t) addr;

        payload += 4 + 8;
    }

    /*
     * MESSAGE-INTEGRITY ( HMAC-SHA1 with the local pwd ).
     *
     * RFC 5389 §15.4: the HMAC input is the STUN message
     * *up to but NOT including* the MESSAGE-INTEGRITY
     * attribute, with the header length field already set as
     * if the 24 byte MI attribute were present.  Hashing the
     * 4 byte MI attribute header as well produces a MAC the
     * browser cannot reproduce, so every connectivity check
     * is answered with a response it discards and ICE fails.
     */
    {
        uint8_t *attr = out + STUN_HEADER_SIZE + payload;

        write_be16(attr, 0x0008);
        write_be16(attr + 2, 20);
        write_be16(out + 2, (uint16_t) (payload + 24));

        unsigned char mac[EVP_MAX_MD_SIZE];
        unsigned int mac_len = 0;

        HMAC(EVP_sha1(),
             local_pwd, (int) strlen(local_pwd),
             out, STUN_HEADER_SIZE + payload,
             mac, &mac_len);

        memcpy(attr + 4, mac, 20);
        payload += 24;
    }

    /*
     * FINGERPRINT ( CRC32 over the message with the length
     * including the fingerprint attribute ).
     */
    {
        uint8_t *attr = out + STUN_HEADER_SIZE + payload;

        /*
         * FINGERPRINT is attribute 0x8028 (RFC 5389 §18.2).
         * 0x0028 is in the comprehension-REQUIRED range, so a
         * peer that does not know it MUST reject the whole
         * message.
         */
        write_be16(attr, 0x8028);
        write_be16(attr + 2, 4);
        write_be16(out + 2, (uint16_t) (payload + 8));

        /*
         * RFC 5389 §15.5: the CRC covers the message up to
         * but not including the FINGERPRINT attribute, with
         * the length field already counting it.
         */
        uint32_t crc = crc32_stun(out, STUN_HEADER_SIZE + payload) ^
                       0x5354554EUL;

        attr[4] = (uint8_t) (crc >> 24);
        attr[5] = (uint8_t) (crc >> 16);
        attr[6] = (uint8_t) (crc >> 8);
        attr[7] = (uint8_t) crc;

        payload += 8;
    }

    *out_len = STUN_HEADER_SIZE + payload;

    return 0;
}

int stun_verify_mi(const uint8_t *buf, size_t len,
                   const char *password)
{
    if (buf == NULL || password == NULL || password[0] == 0 ||
        len < STUN_HEADER_SIZE + 24) {
        return 0;
    }

    const uint8_t *mi = NULL;
    size_t mi_len = 0;

    if (!stun_find_attribute(buf, len, 0x0008, &mi, &mi_len) ||
        mi_len != 20) {
        return 0;
    }

    /*
     * RFC 5389 §15.4: the MAC covers the message from the
     * start of the header up to the start of the
     * MESSAGE-INTEGRITY *attribute header* (4 bytes before
     * the value `mi` points at) — the attribute itself is
     * excluded, as is any FINGERPRINT that follows it.
     *
     * Crucially the header length field used in the hash must
     * be the value it had when the sender computed the MAC:
     * everything up to and including MI, i.e. the message
     * truncated right after this attribute.  When a
     * FINGERPRINT follows, the on-the-wire length field is
     * 8 bytes larger, so hashing the buffer verbatim yields
     * the wrong MAC and every browser check is rejected with
     * a bogus 401.  Hash over a patched copy instead.
     */
    size_t mi_header = (size_t) (mi - buf) - 4;

    if (mi_header < STUN_HEADER_SIZE || mi_header + 24 > len) {
        return 0;
    }

    /*
     * Connectivity checks are a few hundred bytes at most; the stack
     * copy keeps this path free of allocation.
     */
    uint8_t stack_scratch[512];
    uint8_t *scratch = stack_scratch;
    uint8_t *heap_scratch = NULL;

    if (mi_header > sizeof(stack_scratch)) {
        heap_scratch = malloc(mi_header);
        scratch = heap_scratch;

        if (scratch == NULL) {
            return 0;
        }
    }

    memcpy(scratch, buf, mi_header);
    write_be16(scratch + 2, (uint16_t) (mi_header + 24 - STUN_HEADER_SIZE));

    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int mac_len = 0;

    int verified = HMAC(EVP_sha1(), password, (int) strlen(password),
                        scratch, mi_header, mac, &mac_len) != NULL &&
                   mac_len == 20 &&
                   CRYPTO_memcmp(mac, mi, 20) == 0;

    free(heap_scratch);

    /* CRYPTO_memcmp is constant time: this MAC guards the peer slot. */
    return verified;
}

int stun_build_error_response(const uint8_t tid[12],
                              uint16_t error_code,
                              uint8_t *out,
                              size_t out_capacity,
                              size_t *out_len)
{
    /* 20 header + 4+8 SOFTWARE + 4+8 ERROR-CODE */
    if (out == NULL || out_len == NULL || out_capacity < 44) {
        return -1;
    }

    memset(out, 0, 44);

    out[0] = 0x01;
    out[1] = 0x11;                       /* binding error response */
    out[4] = 0x21;
    out[5] = 0x12;
    out[6] = 0xA4;
    out[7] = 0x42;                       /* magic cookie */
    if (tid != NULL) {
        memcpy(out + 8, tid, 12);
    }

    size_t payload = 0;

    /* SOFTWARE (RFC 5389 §6.2: SHOULD be present on errors) */
    {
        const char sw[] = "camstream";
        uint8_t *attr = out + STUN_HEADER_SIZE;

        write_be16(attr, 0x8022);
        write_be16(attr + 2, (uint16_t) (sizeof(sw) - 1));
        memcpy(attr + 4, sw, sizeof(sw) - 1);
        payload += 4 + ((sizeof(sw) - 1 + 3) & ~3u);
    }

    /* ERROR-CODE: 1 reserved byte, 1 class, 2 reason, 4 pad */
    {
        uint8_t *attr = out + STUN_HEADER_SIZE + payload;

        write_be16(attr, 0x0009);
        write_be16(attr + 2, 8);
        attr[4] = 0;
        attr[5] = (uint8_t) (error_code / 100);           /* class */
        write_be16(attr + 6, (uint16_t) (error_code % 100)); /* reason */
        payload += 12;
    }

    write_be16(out + 2, (uint16_t) payload);
    *out_len = STUN_HEADER_SIZE + payload;

    return 0;
}
