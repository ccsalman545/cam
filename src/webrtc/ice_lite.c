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
#include <string.h>
#include <unistd.h>

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

    if (buf[0] >= 128 && buf[0] <= 191) {
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
     * RFC 8445 §7.3: the first component is the RECEIVER's
     * ufrag. A browser check therefore arrives as
     * "<server-ufrag>:<browser-ufrag>". Treating our ufrag as
     * the second component (the usual ICE-role mix-up) would
     * drop every well-formed check.
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
    if (request_len < STUN_HEADER_SIZE || out_capacity < 128) {
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

        write_be16(attr, 0x0020);
        write_be16(attr + 2, 8);
        attr[4] = 0;

        if (peer->ss_family == AF_INET6) {
            attr[5] = 0x02;

            const struct sockaddr_in6 *a6 =
                (const struct sockaddr_in6 *) peer;

            uint16_t xport = ntohs(a6->sin6_port) ^ 0x2112;
            write_be16(attr + 6, xport);

            const uint8_t cookie[4] = {
                0x21, 0x12, 0xA4, 0x42
            };

            for (int i = 0; i < 16; i++) {
                attr[8 + i] = ((const uint8_t *) &a6->sin6_addr)[i] ^
                              (i < 4 ? cookie[i] : request[8 + (i - 4)]);
            }
        } else {
            attr[5] = 0x01;

            const struct sockaddr_in *a4 = (const struct sockaddr_in *) peer;

            uint16_t xport = ntohs(a4->sin_port) ^ 0x2112;
            write_be16(attr + 6, xport);

            uint32_t addr = ntohl(a4->sin_addr.s_addr) ^ STUN_MAGIC_COOKIE;

            attr[8] = (uint8_t) (addr >> 24);
            attr[9] = (uint8_t) (addr >> 16);
            attr[10] = (uint8_t) (addr >> 8);
            attr[11] = (uint8_t) addr;
        }

        payload += 4 + 8;
    }

    /*
     * MESSAGE-INTEGRITY ( HMAC-SHA1 with the local pwd ).
     * The length field in the header must include the MI
     * attribute while computing the MAC.
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
             out, STUN_HEADER_SIZE + payload + 4,
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

        write_be16(attr, 0x0028);
        write_be16(attr + 2, 4);
        write_be16(out + 2, (uint16_t) (payload + 8));

        uint32_t crc = crc32_stun(out, STUN_HEADER_SIZE + payload + 4) ^
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
