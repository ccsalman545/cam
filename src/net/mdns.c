#define _POSIX_C_SOURCE 200809L

/*
 * mdns.c
 *
 * Multicast DNS responder. See include/net/mdns.h for what is published
 * and which parts of RFC 6762 are deliberately left out.
 *
 * Packets here are built without name compression: the record set is
 * fixed and small (at most eight A records plus four others), so a
 * response stays around three hundred bytes, well inside the limit, and
 * no pointer bookkeeping is needed. Incoming names are decompressed,
 * because queries from real resolvers do use pointers.
 */
#include "mdns.h"

#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "log.h"

#define MDNS_GROUP "224.0.0.251"

/* The protocol's well known port: a query from any other source port is
 * a legacy unicast resolver (RFC 6762 section 6.7). This is not the
 * configured port, because the responder may be moved off 5353 to share
 * a host with another responder. */
#define MDNS_PORT 5353

/* RFC 6762 section 10: 120 s for unique records, 75 min for shared ones. */
#define MDNS_TTL_UNIQUE 120
#define MDNS_TTL_SHARED 4500
/* Section 6.7: a legacy unicast reply must not be cached for long. */
#define MDNS_TTL_LEGACY 10

/* Section 8: three probes 250 ms apart, then two announcements 1 s apart. */
#define MDNS_PROBE_COUNT 3
#define MDNS_PROBE_INTERVAL_MS 250
#define MDNS_ANNOUNCE_COUNT 2
#define MDNS_ANNOUNCE_INTERVAL_MS 1000

/* Section 6: multicast responses wait 20 to 120 ms to spread a burst. */
#define MDNS_REPLY_DELAY_MIN_MS 20
#define MDNS_REPLY_DELAY_SPAN_MS 100

#define MDNS_RESCAN_INTERVAL_MS 2000
#define MDNS_CONFLICT_INTERVAL_MS 1000
#define MDNS_NAME_SUFFIX_MAX 9

#define MDNS_PACKET_MAX 1400
#define MDNS_QUESTIONS_MAX 8
#define MDNS_RECORDS_MAX 64
#define MDNS_RDATA_MAX 256
#define MDNS_TXT_STRING_MAX 200

#define MDNS_TYPE_A 1
#define MDNS_TYPE_PTR 12
#define MDNS_TYPE_TXT 16
#define MDNS_TYPE_SRV 33
#define MDNS_TYPE_ANY 255

#define MDNS_CLASS_IN 1
#define MDNS_CLASS_ANY 255
#define MDNS_CLASS_FLUSH 0x8000
#define MDNS_QU_BIT 0x8000

#define MDNS_FLAG_RESPONSE 0x8000
#define MDNS_FLAG_AUTHORITATIVE 0x0400
#define MDNS_OPCODE_MASK 0x7800

/* Answer groups, one bit per group of records this responder owns. */
#define GROUP_A 0x01
#define GROUP_PTR 0x02
#define GROUP_SRV_TXT 0x04
#define GROUP_SERVICE_TYPES 0x08
#define GROUP_ALL (GROUP_A | GROUP_PTR | GROUP_SRV_TXT | GROUP_SERVICE_TYPES)

typedef enum {
    MDNS_PROBING = 0,
    MDNS_ANNOUNCING,
    MDNS_IDLE,
    MDNS_OFF
} MdnsState;

struct MdnsResponder {
    MdnsConfig config;

    int fd;
    MdnsState state;

    /* Room for the label plus a "-10" rename suffix; a DNS label is
     * limited to 63 characters by sanitize_label() below. */
    char label[80];
    int suffix;                 /* 0 = label, n = label-(n+1) after conflicts */

    int probes_left;
    int announces_left;
    uint64_t next_event_ms;

    uint16_t pending_mask;      /* groups collected for a multicast reply */
    uint64_t pending_ms;        /* 0 = nothing pending */

    uint64_t next_scan_ms;
    uint64_t last_conflict_ms;
    uint32_t random_state;
    int socket_warned;          /* one log line per responder, not per packet */

    char addresses[MDNS_MAX_ADDRESSES][INET_ADDRSTRLEN];
    size_t address_count;

    MdnsStats stats;
};

/* ------------------------------------------------------------------ */
/* Byte order helpers                                                  */
/* ------------------------------------------------------------------ */

static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t) (((uint16_t) p[0] << 8) | p[1]);
}

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
           ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}

static void write_be16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t) (value >> 8);
    p[1] = (uint8_t) (value & 0xFF);
}

static void write_be32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t) (value >> 24);
    p[1] = (uint8_t) ((value >> 16) & 0xFF);
    p[2] = (uint8_t) ((value >> 8) & 0xFF);
    p[3] = (uint8_t) (value & 0xFF);
}

static uint64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t) ts.tv_sec * 1000ULL + (uint64_t) ts.tv_nsec / 1000000ULL;
}

/* ------------------------------------------------------------------ */
/* Name encoding and decoding                                          */
/* ------------------------------------------------------------------ */

/* Writes a dotted name as length prefixed labels. Returns the offset
 * behind the name, or -1 when it does not fit or is malformed. */
static int dns_write_name(uint8_t *buffer,
                          size_t capacity,
                          size_t offset,
                          const char *name)
{
    const char *label = name;

    while (*label != 0) {
        const char *dot = strchr(label, '.');

        size_t label_length = dot != NULL ? (size_t) (dot - label)
                                          : strlen(label);

        if (label_length == 0 || label_length > 63) {
            return -1;
        }

        if (offset + 1 + label_length + 1 > capacity) {
            return -1;
        }

        buffer[offset++] = (uint8_t) label_length;
        memcpy(buffer + offset, label, label_length);
        offset += label_length;

        if (dot == NULL) {
            break;
        }

        label = dot + 1;
    }

    if (offset + 1 > capacity) {
        return -1;
    }

    buffer[offset++] = 0;

    return (int) offset;
}

/*
 * Reads a name, following compression pointers. *next_offset is the
 * position behind the name in the packet, which is what the caller needs
 * to continue parsing even when the name ended in a pointer.
 */
static int dns_read_name(const uint8_t *packet,
                         size_t length,
                         size_t offset,
                         char *out,
                         size_t out_size,
                         size_t *next_offset)
{
    size_t position = offset;
    size_t out_length = 0;
    int followed_pointer = 0;
    int jumps = 0;

    for (;;) {
        if (position >= length) {
            return -1;
        }

        uint8_t label_length = packet[position];

        if (label_length == 0) {
            if (!followed_pointer) {
                *next_offset = position + 1;
            }
            break;
        }

        if ((label_length & 0xC0) == 0xC0) {
            if (position + 1 >= length || ++jumps > 8) {
                return -1;
            }

            size_t pointer = (size_t) (((label_length & 0x3F) << 8) |
                                       packet[position + 1]);

            if (pointer >= length) {
                return -1;
            }

            if (!followed_pointer) {
                *next_offset = position + 2;
                followed_pointer = 1;
            }

            position = pointer;
            continue;
        }

        if ((label_length & 0xC0) != 0 || label_length > 63 ||
            position + 1 + label_length > length) {
            return -1;
        }

        size_t needed = out_length + (out_length > 0 ? 1 : 0) + label_length + 1;

        if (needed > out_size) {
            return -1;
        }

        if (out_length > 0) {
            out[out_length++] = '.';
        }

        memcpy(out + out_length, packet + position + 1, label_length);
        out_length += label_length;
        position += 1 + label_length;
    }

    out[out_length] = 0;

    return 0;
}

/* ------------------------------------------------------------------ */
/* Record data                                                         */
/* ------------------------------------------------------------------ */

static size_t build_a_rdata(uint8_t *out, size_t capacity, const char *ip)
{
    struct in_addr addr;

    if (capacity < 4 || inet_pton(AF_INET, ip, &addr) != 1) {
        return 0;
    }

    memcpy(out, &addr.s_addr, 4);

    return 4;
}

static size_t build_name_rdata(uint8_t *out, size_t capacity, const char *name)
{
    int written = dns_write_name(out, capacity, 0, name);

    return written > 0 ? (size_t) written : 0;
}

static size_t build_srv_rdata(uint8_t *out,
                              size_t capacity,
                              uint16_t port,
                              const char *target)
{
    if (capacity < 8) {
        return 0;
    }

    write_be16(out + 0, 0);         /* priority */
    write_be16(out + 2, 0);         /* weight */
    write_be16(out + 4, port);

    int written = dns_write_name(out, capacity, 6, target);

    return written > 0 ? (size_t) written : 0;
}

/*
 * TXT is a list of length prefixed strings. model and path are what a
 * DNS-SD browser displays and what a human reads in "avahi-browse -a".
 */
static size_t build_txt_rdata(uint8_t *out, size_t capacity, const char *model)
{
    char entries[MDNS_TXT_STRING_MAX];

    int written = snprintf(entries, sizeof(entries), "model=%s", model);

    if (written <= 0 || (size_t) written >= sizeof(entries)) {
        return 0;
    }

    size_t model_length = (size_t) written;
    static const char path[] = "path=/";
    size_t path_length = sizeof(path) - 1;

    if (model_length > 255 || capacity < 2 + model_length + path_length) {
        return 0;
    }

    size_t offset = 0;

    out[offset++] = (uint8_t) model_length;
    memcpy(out + offset, entries, model_length);
    offset += model_length;

    out[offset++] = (uint8_t) path_length;
    memcpy(out + offset, path, path_length);
    offset += path_length;

    return offset;
}

static void build_names(const MdnsResponder *responder,
                        char *host,
                        size_t host_size,
                        char *instance,
                        size_t instance_size)
{
    snprintf(host, host_size, "%s.local", responder->label);
    snprintf(instance, instance_size, "%s._http._tcp.local", responder->label);
}

static int append_record(uint8_t *buffer,
                         size_t capacity,
                         size_t offset,
                         const char *name,
                         uint16_t type,
                         uint16_t klass,
                         uint32_t ttl,
                         const uint8_t *rdata,
                         size_t rdata_length)
{
    int name_end = dns_write_name(buffer, capacity, offset, name);

    if (name_end < 0) {
        return -1;
    }

    size_t position = (size_t) name_end;

    if (position + 10 + rdata_length > capacity) {
        return -1;
    }

    write_be16(buffer + position, type);
    write_be16(buffer + position + 2, klass);
    write_be32(buffer + position + 4, ttl);
    write_be16(buffer + position + 8, (uint16_t) rdata_length);
    memcpy(buffer + position + 10, rdata, rdata_length);

    return (int) (position + 10 + rdata_length);
}

/*
 * Appends the records of the requested groups. Records this responder
 * owns alone (A, SRV, TXT) carry the cache flush bit; the shared PTR
 * records must not, or a resolver would drop every other responder's
 * announcement of the same service type.
 */
static int build_record_set(MdnsResponder *responder,
                            uint8_t *buffer,
                            size_t capacity,
                            size_t offset,
                            uint16_t mask,
                            uint32_t ttl_unique,
                            uint32_t ttl_shared,
                            uint16_t *record_count)
{
    char host[MDNS_NAME_MAX];
    char instance[MDNS_NAME_MAX];

    build_names(responder, host, sizeof(host), instance, sizeof(instance));

    uint8_t rdata[MDNS_RDATA_MAX];
    uint16_t records = 0;

    if ((mask & GROUP_A) != 0) {
        for (size_t i = 0; i < responder->address_count; i++) {
            size_t rdata_length = build_a_rdata(rdata, sizeof(rdata),
                                                responder->addresses[i]);

            if (rdata_length == 0) {
                continue;
            }

            int end = append_record(buffer, capacity, offset, host,
                                    MDNS_TYPE_A,
                                    MDNS_CLASS_IN | MDNS_CLASS_FLUSH,
                                    ttl_unique, rdata, rdata_length);

            if (end < 0) {
                return -1;
            }

            offset = (size_t) end;
            records++;
        }
    }

    if ((mask & GROUP_PTR) != 0) {
        size_t rdata_length = build_name_rdata(rdata, sizeof(rdata), instance);

        if (rdata_length == 0) {
            return -1;
        }

        int end = append_record(buffer, capacity, offset, "_http._tcp.local",
                                MDNS_TYPE_PTR, MDNS_CLASS_IN, ttl_shared, rdata,
                                rdata_length);

        if (end < 0) {
            return -1;
        }

        offset = (size_t) end;
        records++;
    }

    if ((mask & GROUP_SRV_TXT) != 0) {
        size_t rdata_length = build_srv_rdata(rdata, sizeof(rdata),
                                              responder->config.http_port,
                                              host);

        if (rdata_length == 0) {
            return -1;
        }

        int end = append_record(buffer, capacity, offset, instance,
                                MDNS_TYPE_SRV,
                                MDNS_CLASS_IN | MDNS_CLASS_FLUSH, ttl_unique,
                                rdata, rdata_length);

        if (end < 0) {
            return -1;
        }

        offset = (size_t) end;
        records++;

        rdata_length = build_txt_rdata(rdata, sizeof(rdata),
                                       responder->config.model);

        if (rdata_length == 0) {
            return -1;
        }

        end = append_record(buffer, capacity, offset, instance, MDNS_TYPE_TXT,
                            MDNS_CLASS_IN | MDNS_CLASS_FLUSH, ttl_unique, rdata,
                            rdata_length);

        if (end < 0) {
            return -1;
        }

        offset = (size_t) end;
        records++;
    }

    if ((mask & GROUP_SERVICE_TYPES) != 0) {
        size_t rdata_length = build_name_rdata(rdata, sizeof(rdata),
                                               "_http._tcp.local");

        if (rdata_length == 0) {
            return -1;
        }

        int end = append_record(buffer, capacity, offset,
                                "_services._dns-sd._udp.local", MDNS_TYPE_PTR,
                                MDNS_CLASS_IN, ttl_shared, rdata, rdata_length);

        if (end < 0) {
            return -1;
        }

        offset = (size_t) end;
        records++;
    }

    *record_count = records;

    return (int) offset;
}

/* Builds a response (or an announcement, which is an unsolicited one). */
static int build_response(MdnsResponder *responder,
                          uint8_t *buffer,
                          size_t capacity,
                          uint16_t id,
                          uint16_t mask,
                          uint32_t ttl_unique,
                          uint32_t ttl_shared,
                          size_t *length_out)
{
    if (capacity < 12) {
        return -1;
    }

    memset(buffer, 0, 12);
    write_be16(buffer + 0, id);
    write_be16(buffer + 2, MDNS_FLAG_RESPONSE | MDNS_FLAG_AUTHORITATIVE);

    uint16_t records = 0;

    int end = build_record_set(responder, buffer, capacity, 12, mask,
                               ttl_unique, ttl_shared, &records);

    if (end < 0) {
        return -1;
    }

    write_be16(buffer + 6, records);        /* ANCOUNT */

    *length_out = (size_t) end;

    return 0;
}

/*
 * A probe is a query whose authority section holds the records the
 * responder wants to claim (RFC 6762 section 8.1). Without it a second
 * responder with the same name would only be noticed after both have
 * announced, which is what the rename procedure exists to avoid.
 */
static int build_probe(MdnsResponder *responder,
                       uint8_t *buffer,
                       size_t capacity,
                       size_t *length_out)
{
    if (capacity < 12) {
        return -1;
    }

    memset(buffer, 0, 12);

    uint16_t records = 0;

    int end = build_record_set(responder, buffer, capacity, 12,
                               GROUP_A | GROUP_SRV_TXT, MDNS_TTL_UNIQUE,
                               MDNS_TTL_SHARED, &records);

    if (end < 0) {
        return -1;
    }

    write_be16(buffer + 8, records);        /* NSCOUNT */

    *length_out = (size_t) end;

    return 0;
}

/* ------------------------------------------------------------------ */
/* Sending                                                             */
/* ------------------------------------------------------------------ */

static void warn_socket_once(MdnsResponder *responder, const char *what)
{
    if (responder->socket_warned) {
        return;
    }

    responder->socket_warned = 1;
    log_warn("mdns", "%s failed: errno=%d (%s); further failures are only "
                     "counted, see /api/status", what, errno, strerror(errno));
}

static void send_to_peer(MdnsResponder *responder,
                         const uint8_t *data,
                         size_t length,
                         const struct sockaddr_in *peer)
{
    if (responder->config.send != NULL) {
        responder->config.send(responder->config.send_user, data, length, peer);
        return;
    }

    if (responder->fd < 0) {
        return;
    }

    ssize_t sent = sendto(responder->fd, data, length, 0,
                          (const struct sockaddr *) peer, sizeof(*peer));

    if (sent < 0) {
        responder->stats.socket_errors++;
        warn_socket_once(responder, "sendto (unicast reply)");
    }
}

/*
 * Sends one packet to the group on every interface. A host with both
 * Ethernet and Wi-Fi would otherwise send on the default route only and
 * stay invisible on the other network, which is exactly the case a Pi
 * used over a cable and over Wi-Fi at the same time.
 */
static void send_to_group(MdnsResponder *responder,
                          const uint8_t *data,
                          size_t length)
{
    if (responder->config.send != NULL) {
        responder->config.send(responder->config.send_user, data, length, NULL);
        return;
    }

    if (responder->fd < 0) {
        return;
    }

    struct sockaddr_in group;

    memset(&group, 0, sizeof(group));
    group.sin_family = AF_INET;
    group.sin_port = htons(responder->config.port);
    inet_pton(AF_INET, MDNS_GROUP, &group.sin_addr);

    for (size_t i = 0; i < responder->address_count; i++) {
        struct in_addr local;

        if (inet_pton(AF_INET, responder->addresses[i], &local) != 1) {
            continue;
        }

        if (setsockopt(responder->fd, IPPROTO_IP, IP_MULTICAST_IF, &local,
                       sizeof(local)) != 0) {
            responder->stats.socket_errors++;
            warn_socket_once(responder, "setsockopt IP_MULTICAST_IF");
            continue;
        }

        ssize_t sent = sendto(responder->fd, data, length, 0,
                              (const struct sockaddr *) &group, sizeof(group));

        if (sent < 0) {
            responder->stats.socket_errors++;
            warn_socket_once(responder, "sendto (multicast)");
        }
    }
}

/* ------------------------------------------------------------------ */
/* Interfaces                                                          */
/* ------------------------------------------------------------------ */

static void set_membership(MdnsResponder *responder, const char *ip, int join)
{
    if (responder->fd < 0) {
        return;
    }

    struct ip_mreq request;

    memset(&request, 0, sizeof(request));

    if (inet_pton(AF_INET, MDNS_GROUP, &request.imr_multiaddr) != 1 ||
        inet_pton(AF_INET, ip, &request.imr_interface) != 1) {
        return;
    }

    int option = join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP;

    if (setsockopt(responder->fd, IPPROTO_IP, option, &request,
                   sizeof(request)) != 0) {
        log_warn("mdns", "cannot %s the mDNS group on %s: errno=%d (%s)",
                 join ? "join" : "leave", ip, errno, strerror(errno));
    }
}

static int address_is_ours(const MdnsResponder *responder, uint32_t ip_be)
{
    for (size_t i = 0; i < responder->address_count; i++) {
        struct in_addr addr;

        if (inet_pton(AF_INET, responder->addresses[i], &addr) == 1 &&
            addr.s_addr == ip_be) {
            return 1;
        }
    }

    return 0;
}

/*
 * Collects the non-loopback IPv4 addresses that are up. Returns 1 when
 * the set changed, in which case memberships are updated and the caller
 * re-announces: a cable plugged in after boot must become visible
 * without a restart.
 */
static int scan_addresses(MdnsResponder *responder)
{
    struct ifaddrs *addrs = NULL;

    if (getifaddrs(&addrs) != 0) {
        responder->stats.socket_errors++;

        if (!responder->socket_warned) {
            responder->socket_warned = 1;
            log_warn("mdns", "getifaddrs failed: errno=%d (%s)", errno,
                     strerror(errno));
        }

        return 0;
    }

    char found[MDNS_MAX_ADDRESSES][INET_ADDRSTRLEN];
    size_t found_count = 0;

    for (struct ifaddrs *ifa = addrs;
         ifa != NULL && found_count < MDNS_MAX_ADDRESSES;
         ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET ||
            (ifa->ifa_flags & IFF_UP) == 0 ||
            (ifa->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }

        const struct sockaddr_in *addr =
            (const struct sockaddr_in *) ifa->ifa_addr;

        char text[INET_ADDRSTRLEN];

        if (inet_ntop(AF_INET, &addr->sin_addr, text, sizeof(text)) == NULL) {
            continue;
        }

        int duplicate = 0;

        for (size_t i = 0; i < found_count; i++) {
            if (strcmp(found[i], text) == 0) {
                duplicate = 1;
                break;
            }
        }

        if (duplicate) {
            continue;
        }

        memcpy(found[found_count], text, INET_ADDRSTRLEN);
        found_count++;
    }

    freeifaddrs(addrs);

    int changed = found_count != responder->address_count;

    for (size_t i = 0; i < found_count && !changed; i++) {
        int known = 0;

        for (size_t n = 0; n < responder->address_count; n++) {
            if (strcmp(found[i], responder->addresses[n]) == 0) {
                known = 1;
                break;
            }
        }

        changed = !known;
    }

    if (!changed) {
        return 0;
    }

    for (size_t i = 0; i < responder->address_count; i++) {
        int still_there = 0;

        for (size_t n = 0; n < found_count; n++) {
            if (strcmp(found[i], found[n]) == 0) {
                still_there = 1;
                break;
            }
        }

        if (!still_there) {
            set_membership(responder, responder->addresses[i], 0);
        }
    }

    for (size_t i = 0; i < found_count; i++) {
        int was_there = 0;

        for (size_t n = 0; n < responder->address_count; n++) {
            if (strcmp(found[i], responder->addresses[n]) == 0) {
                was_there = 1;
                break;
            }
        }

        if (!was_there) {
            set_membership(responder, found[i], 1);
        }

        /*
         * Exactly one INET_ADDRSTRLEN each and always NUL terminated by
         * inet_ntop, so this is a full copy rather than a text copy. A
         * snprintf here makes the compiler reason about strings that
         * could span the whole array and warn about truncation.
         */
        memcpy(responder->addresses[i], found[i], INET_ADDRSTRLEN);
    }

    responder->address_count = found_count;
    responder->stats.address_count = found_count;

    return 1;
}

/* ------------------------------------------------------------------ */
/* Queries                                                             */
/* ------------------------------------------------------------------ */

static uint16_t answer_mask(const MdnsResponder *responder,
                            const char *name,
                            uint16_t qtype)
{
    char host[MDNS_NAME_MAX];
    char instance[MDNS_NAME_MAX];

    build_names(responder, host, sizeof(host), instance, sizeof(instance));

    uint16_t mask = 0;

    int wants_all = qtype == MDNS_TYPE_ANY;

    if (strcasecmp(name, host) == 0 && (wants_all || qtype == MDNS_TYPE_A)) {
        mask |= GROUP_A;
    }

    /*
     * A service type browse is followed by instance resolution, so the
     * SRV and TXT records ride along as additional records; resolvers
     * need one round trip less, which matters on a slow LAN.
     */
    if (strcasecmp(name, "_http._tcp.local") == 0 &&
        (wants_all || qtype == MDNS_TYPE_PTR)) {
        mask |= GROUP_PTR | GROUP_SRV_TXT;
    }

    if (strcasecmp(name, instance) == 0 &&
        (wants_all || qtype == MDNS_TYPE_SRV || qtype == MDNS_TYPE_TXT)) {
        mask |= GROUP_SRV_TXT;
    }

    if (strcasecmp(name, "_services._dns-sd._udp.local") == 0 &&
        (wants_all || qtype == MDNS_TYPE_PTR)) {
        mask |= GROUP_SERVICE_TYPES;
    }

    return mask;
}

/*
 * Known answer suppression (RFC 6762 section 7.1): a group is dropped
 * when the querier already holds those records with at least half the
 * remaining TTL. Without this every client repeating a query keeps the
 * whole LAN answering.
 */
static uint16_t suppress_known_answers(const MdnsResponder *responder,
                                       const uint8_t *packet,
                                       size_t length,
                                       size_t offset,
                                       uint16_t count)
{
    char host[MDNS_NAME_MAX];
    char instance[MDNS_NAME_MAX];

    build_names(responder, host, sizeof(host), instance, sizeof(instance));

    uint8_t expected[MDNS_RDATA_MAX];
    size_t addresses_known = 0;
    int ptr_known = 0;
    int srv_known = 0;
    int txt_known = 0;
    int services_known = 0;

    for (uint16_t i = 0; i < count; i++) {
        char name[MDNS_NAME_MAX];
        size_t next = 0;

        if (dns_read_name(packet, length, offset, name, sizeof(name),
                          &next) != 0 || next + 10 > length) {
            break;
        }

        uint16_t type = read_be16(packet + next);
        uint32_t ttl = read_be32(packet + next + 4);
        uint16_t rdata_length = read_be16(packet + next + 8);
        size_t rdata_offset = next + 10;

        if (rdata_offset + rdata_length > length) {
            break;
        }

        if (type == MDNS_TYPE_A && strcasecmp(name, host) == 0 &&
            rdata_length == 4 && ttl >= MDNS_TTL_UNIQUE / 2) {
            for (size_t n = 0; n < responder->address_count; n++) {
                size_t expected_length = build_a_rdata(expected,
                                                       sizeof(expected),
                                                       responder->addresses[n]);

                if (expected_length == 4 &&
                    memcmp(expected, packet + rdata_offset, 4) == 0) {
                    addresses_known++;
                    break;
                }
            }
        } else if (type == MDNS_TYPE_PTR &&
                   strcasecmp(name, "_http._tcp.local") == 0 &&
                   ttl >= MDNS_TTL_SHARED / 2 &&
                   rdata_length < MDNS_RDATA_MAX) {
            char target[MDNS_NAME_MAX];

            if (dns_read_name(packet, length, rdata_offset, target,
                              sizeof(target), &next) == 0 &&
                strcasecmp(target, instance) == 0) {
                ptr_known = 1;
            }
        } else if (type == MDNS_TYPE_SRV && strcasecmp(name, instance) == 0 &&
                   ttl >= MDNS_TTL_UNIQUE / 2) {
            size_t expected_length = build_srv_rdata(expected, sizeof(expected),
                                                     responder->config.http_port,
                                                     host);

            if (expected_length == rdata_length &&
                memcmp(expected, packet + rdata_offset, rdata_length) == 0) {
                srv_known = 1;
            }
        } else if (type == MDNS_TYPE_TXT && strcasecmp(name, instance) == 0 &&
                   ttl >= MDNS_TTL_UNIQUE / 2) {
            size_t expected_length = build_txt_rdata(expected, sizeof(expected),
                                                     responder->config.model);

            if (expected_length == rdata_length &&
                memcmp(expected, packet + rdata_offset, rdata_length) == 0) {
                txt_known = 1;
            }
        } else if (type == MDNS_TYPE_PTR &&
                   strcasecmp(name, "_services._dns-sd._udp.local") == 0 &&
                   ttl >= MDNS_TTL_SHARED / 2) {
            char target[MDNS_NAME_MAX];

            if (dns_read_name(packet, length, rdata_offset, target,
                              sizeof(target), &next) == 0 &&
                strcasecmp(target, "_http._tcp.local") == 0) {
                services_known = 1;
            }
        }

        offset = rdata_offset + rdata_length;
    }

    uint16_t suppressed = 0;

    if (addresses_known == responder->address_count &&
        responder->address_count > 0) {
        suppressed |= GROUP_A;
    }

    if (ptr_known) {
        suppressed |= GROUP_PTR;
    }

    if (srv_known && txt_known) {
        suppressed |= GROUP_SRV_TXT;
    }

    if (services_known) {
        suppressed |= GROUP_SERVICE_TYPES;
    }

    return suppressed;
}

/* ------------------------------------------------------------------ */
/* Conflict handling                                                   */
/* ------------------------------------------------------------------ */

/*
 * True when a record received from the network carries exactly the data
 * this responder publishes, which RFC 6762 section 9 defines as "not a
 * conflict". Unknown record types are treated as not ours, so they do
 * not mask a real conflict.
 */
static int record_matches_ours(const MdnsResponder *responder,
                               const char *host,
                               const char *instance,
                               const char *name,
                               uint16_t type,
                               const uint8_t *rdata,
                               size_t rdata_length)
{
    uint8_t expected[MDNS_RDATA_MAX];

    if (type == MDNS_TYPE_A && strcasecmp(name, host) == 0) {
        for (size_t i = 0; i < responder->address_count; i++) {
            size_t length = build_a_rdata(expected, sizeof(expected),
                                          responder->addresses[i]);

            if (length == rdata_length &&
                memcmp(expected, rdata, rdata_length) == 0) {
                return 1;
            }
        }

        return 0;
    }

    if (type == MDNS_TYPE_SRV && strcasecmp(name, instance) == 0) {
        size_t length = build_srv_rdata(expected, sizeof(expected),
                                        responder->config.http_port, host);

        return length == rdata_length &&
               memcmp(expected, rdata, rdata_length) == 0;
    }

    if (type == MDNS_TYPE_TXT && strcasecmp(name, instance) == 0) {
        size_t length = build_txt_rdata(expected, sizeof(expected),
                                        responder->config.model);

        return length == rdata_length &&
               memcmp(expected, rdata, rdata_length) == 0;
    }

    return 0;
}

static void rename_after_conflict(MdnsResponder *responder,
                                  uint64_t time_now,
                                  const char *what)
{
    responder->stats.conflicts++;

    if (time_now - responder->last_conflict_ms < MDNS_CONFLICT_INTERVAL_MS) {
        /* One rename attempt per second: a burst of replies from the
         * same peer must not walk the name through every suffix. */
        return;
    }

    responder->last_conflict_ms = time_now;

    if (responder->suffix >= MDNS_NAME_SUFFIX_MAX) {
        log_error("mdns", "%s.local is taken by another host (%s) and no "
                          "free name was found; the responder is off, set "
                          "mdns_name in the configuration to a unique name",
                  responder->label, what);
        responder->state = MDNS_OFF;
        responder->stats.disabled = 1;
        return;
    }

    responder->suffix++;
    snprintf(responder->label, sizeof(responder->label), "%s-%d",
             responder->config.host, responder->suffix + 1);

    log_warn("mdns", "name conflict (%s): publishing %s.local instead", what,
             responder->label);

    responder->stats.renamed = 1;
    responder->state = MDNS_PROBING;
    responder->probes_left = MDNS_PROBE_COUNT;
    responder->next_event_ms = time_now + MDNS_PROBE_INTERVAL_MS;
}

/*
 * Walks the answer, authority and additional sections of a response
 * looking for records on a name this responder claims. Our own packets
 * are filtered out by the caller, so anything found here is another
 * host's claim.
 */
static void handle_response(MdnsResponder *responder,
                            const uint8_t *packet,
                            size_t length,
                            uint16_t question_count,
                            uint16_t record_count,
                            uint64_t time_now)
{
    char host[MDNS_NAME_MAX];
    char instance[MDNS_NAME_MAX];

    build_names(responder, host, sizeof(host), instance, sizeof(instance));

    size_t offset = 12;

    for (uint16_t i = 0; i < question_count; i++) {
        char name[MDNS_NAME_MAX];
        size_t next = 0;

        if (dns_read_name(packet, length, offset, name, sizeof(name),
                          &next) != 0 || next + 4 > length) {
            return;
        }

        offset = next + 4;
    }

    for (uint16_t i = 0; i < record_count && i < MDNS_RECORDS_MAX; i++) {
        char name[MDNS_NAME_MAX];
        size_t next = 0;

        if (dns_read_name(packet, length, offset, name, sizeof(name),
                          &next) != 0 || next + 10 > length) {
            return;
        }

        uint16_t type = read_be16(packet + next);
        uint16_t rdata_length = read_be16(packet + next + 8);
        size_t rdata_offset = next + 10;

        if (rdata_offset + rdata_length > length) {
            return;
        }

        /*
         * RFC 6762 section 9: a record carrying exactly the same data as
         * ours is our record coming back from a cache, not a conflict.
         * Anything else on the same name is a conflict.
         */
        if (strcasecmp(name, host) == 0 && type == MDNS_TYPE_A) {
            if (record_matches_ours(responder, host, instance, name, type,
                                    packet + rdata_offset,
                                    rdata_length)) {
                offset = rdata_offset + rdata_length;
                continue;
            }

            rename_after_conflict(responder, time_now,
                                  "another host claims the A record");
            return;
        }

        if (strcasecmp(name, instance) == 0 &&
            (type == MDNS_TYPE_SRV || type == MDNS_TYPE_TXT)) {
            if (record_matches_ours(responder, host, instance, name, type,
                                    packet + rdata_offset,
                                    rdata_length)) {
                offset = rdata_offset + rdata_length;
                continue;
            }

            rename_after_conflict(responder, time_now,
                                  "another host claims the service instance");
            return;
        }

        offset = rdata_offset + rdata_length;
    }
}

/* ------------------------------------------------------------------ */
/* Public interface                                                    */
/* ------------------------------------------------------------------ */

static uint32_t next_random(MdnsResponder *responder)
{
    responder->random_state = responder->random_state * 1103515245U + 12345U;

    return (responder->random_state >> 16) & 0x7FFF;
}

static void send_probe(MdnsResponder *responder)
{
    uint8_t buffer[MDNS_PACKET_MAX];
    size_t length = 0;

    if (build_probe(responder, buffer, sizeof(buffer), &length) != 0) {
        responder->stats.socket_errors++;
        return;
    }

    send_to_group(responder, buffer, length);
    responder->stats.announcements++;

    log_debug("mdns", "probing for %s.local", responder->label);
}

static void send_announcement(MdnsResponder *responder)
{
    uint8_t buffer[MDNS_PACKET_MAX];
    size_t length = 0;

    if (build_response(responder, buffer, sizeof(buffer), 0, GROUP_ALL,
                       MDNS_TTL_UNIQUE, MDNS_TTL_SHARED, &length) != 0) {
        responder->stats.socket_errors++;
        return;
    }

    send_to_group(responder, buffer, length);
    responder->stats.announcements++;
}

static void send_goodbye(MdnsResponder *responder)
{
    uint8_t buffer[MDNS_PACKET_MAX];
    size_t length = 0;

    if (responder->state == MDNS_OFF || responder->address_count == 0) {
        return;
    }

    /* TTL 0 tells resolvers to forget the name now instead of in 120 s. */
    if (build_response(responder, buffer, sizeof(buffer), 0, GROUP_ALL, 0, 0,
                       &length) != 0) {
        responder->stats.socket_errors++;
        return;
    }

    send_to_group(responder, buffer, length);
}

static void maybe_answer_multicast(MdnsResponder *responder, uint64_t time_now)
{
    if (responder->pending_ms == 0 || time_now < responder->pending_ms) {
        return;
    }

    uint8_t buffer[MDNS_PACKET_MAX];
    size_t length = 0;

    if (build_response(responder, buffer, sizeof(buffer), 0,
                       responder->pending_mask, MDNS_TTL_UNIQUE,
                       MDNS_TTL_SHARED, &length) == 0) {
        send_to_group(responder, buffer, length);
        responder->stats.responses++;
    } else {
        responder->stats.socket_errors++;
    }

    responder->pending_ms = 0;
    responder->pending_mask = 0;
}

void mdns_handle_packet(MdnsResponder *responder,
                        const uint8_t *packet,
                        size_t length,
                        uint32_t peer_ip_be,
                        uint16_t peer_port,
                        uint64_t time_now)
{
    if (responder == NULL || responder->state == MDNS_OFF) {
        return;
    }

    if (length < 12 || length > MDNS_PACKET_MAX) {
        responder->stats.parse_errors++;
        return;
    }

    uint16_t id = read_be16(packet + 0);
    uint16_t flags = read_be16(packet + 2);
    uint16_t question_count = read_be16(packet + 4);
    uint16_t answer_count = read_be16(packet + 6);
    uint16_t authority_count = read_be16(packet + 8);
    uint16_t additional_count = read_be16(packet + 10);

    if ((flags & MDNS_OPCODE_MASK) != 0 || question_count > MDNS_QUESTIONS_MAX ||
        answer_count > MDNS_RECORDS_MAX || authority_count > MDNS_RECORDS_MAX ||
        additional_count > MDNS_RECORDS_MAX) {
        responder->stats.parse_errors++;
        return;
    }

    if ((flags & MDNS_FLAG_RESPONSE) != 0) {
        handle_response(responder, packet, length, question_count,
                        answer_count + authority_count + additional_count,
                        time_now);
        return;
    }

    if (question_count == 0) {
        return;
    }

    /*
     * RFC 6762 section 6.7: a query from a port other than the mDNS port
     * is a legacy unicast resolver, which cannot handle a multicast
     * answer and expects its transaction ID back. The QU bit asks for
     * the same for one question of an otherwise multicast query.
     */
    int unicast = peer_port != MDNS_PORT;

    uint16_t mask = 0;
    size_t offset = 12;

    for (uint16_t i = 0; i < question_count; i++) {
        char name[MDNS_NAME_MAX];
        size_t next = 0;

        if (dns_read_name(packet, length, offset, name, sizeof(name),
                          &next) != 0 || next + 4 > length) {
            responder->stats.parse_errors++;
            return;
        }

        uint16_t type = read_be16(packet + next);
        uint16_t qclass = read_be16(packet + next + 2);

        offset = next + 4;

        if ((qclass & MDNS_QU_BIT) != 0) {
            unicast = 1;
        }

        uint16_t klass = qclass & 0x7FFF;

        if (klass != MDNS_CLASS_IN && klass != MDNS_CLASS_ANY) {
            continue;
        }

        mask |= answer_mask(responder, name, type);
    }

    responder->stats.queries++;

    /*
     * RFC 6762 section 8.1: the name is not published until probing says
     * it is free, so queries during probing are counted but not answered.
     * The window is a second long at startup, so a browser that opens the
     * page during it simply gets its answer from the next query.
     */
    if (responder->state == MDNS_PROBING) {
        return;
    }

    if (mask == 0) {
        return;
    }

    if (answer_count > 0) {
        mask &= (uint16_t) ~suppress_known_answers(responder, packet, length,
                                                   offset, answer_count);
    }

    if (mask == 0) {
        return;
    }

    if (unicast) {
        uint8_t buffer[MDNS_PACKET_MAX];
        size_t reply_length = 0;

        if (build_response(responder, buffer, sizeof(buffer), id, mask,
                           MDNS_TTL_LEGACY, MDNS_TTL_LEGACY,
                           &reply_length) != 0) {
            responder->stats.socket_errors++;
            return;
        }

        struct sockaddr_in peer;

        memset(&peer, 0, sizeof(peer));
        peer.sin_family = AF_INET;
        peer.sin_port = htons(peer_port);
        peer.sin_addr.s_addr = peer_ip_be;

        send_to_peer(responder, buffer, reply_length, &peer);
        responder->stats.responses++;
        return;
    }

    /* Multicast answers wait a little; the earliest deadline wins so a
     * second query inside the window does not push the reply back. */
    if (responder->pending_ms == 0) {
        uint32_t jitter = next_random(responder) % MDNS_REPLY_DELAY_SPAN_MS;

        responder->pending_ms = time_now + MDNS_REPLY_DELAY_MIN_MS + jitter;
    }

    responder->pending_mask |= mask;
}

void mdns_service(MdnsResponder *responder, uint64_t time_now)
{
    if (responder == NULL) {
        return;
    }

    if (time_now >= responder->next_scan_ms) {
        responder->next_scan_ms = time_now + MDNS_RESCAN_INTERVAL_MS;

        if (scan_addresses(responder) && responder->address_count > 0) {
            log_info("mdns", "%s.local is now announced on %zu address(es), "
                             "first %s",
                     responder->label, responder->address_count,
                     responder->addresses[0]);

            if (responder->state == MDNS_IDLE ||
                responder->state == MDNS_ANNOUNCING) {
                responder->state = MDNS_ANNOUNCING;
                responder->announces_left = MDNS_ANNOUNCE_COUNT;
                responder->next_event_ms = time_now;
            }
        }
    }

    maybe_answer_multicast(responder, time_now);

    if (responder->state == MDNS_OFF) {
        return;
    }

    if (time_now < responder->next_event_ms) {
        return;
    }

    switch (responder->state) {
    case MDNS_PROBING:
        if (responder->probes_left > 0) {
            send_probe(responder);
            responder->probes_left--;
            responder->next_event_ms = time_now + MDNS_PROBE_INTERVAL_MS;

            if (responder->probes_left == 0) {
                /* One more interval of silence before claiming the name. */
                responder->state = MDNS_ANNOUNCING;
                responder->announces_left = MDNS_ANNOUNCE_COUNT;
                responder->next_event_ms = time_now + MDNS_PROBE_INTERVAL_MS;
            }
        }
        break;

    case MDNS_ANNOUNCING:
        if (responder->announces_left > 0) {
            send_announcement(responder);
            responder->announces_left--;
            responder->next_event_ms = time_now + MDNS_ANNOUNCE_INTERVAL_MS;
        }

        if (responder->announces_left == 0) {
            responder->state = MDNS_IDLE;
        }
        break;

    case MDNS_IDLE:
    case MDNS_OFF:
    default:
        break;
    }
}

void mdns_readable(MdnsResponder *responder, uint64_t time_now)
{
    if (responder == NULL || responder->fd < 0) {
        return;
    }

    for (;;) {
        uint8_t buffer[MDNS_PACKET_MAX];
        struct sockaddr_in peer;
        socklen_t peer_length = sizeof(peer);
        ssize_t received = 0;

        memset(&peer, 0, sizeof(peer));

        received = recvfrom(responder->fd, buffer, sizeof(buffer), MSG_DONTWAIT,
                            (struct sockaddr *) &peer, &peer_length);

        if (received < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                responder->stats.socket_errors++;
                warn_socket_once(responder, "recvfrom");
            }

            return;
        }

        if (received == 0 || peer.sin_family != AF_INET) {
            continue;
        }

        /*
         * Our own packets come back on the group unless loopback is
         * disabled, and a second responder on the same host shares the
         * port. Both are filtered by source address and port so they are
         * never mistaken for a conflicting claim on our name.
         */
        if (ntohs(peer.sin_port) == responder->config.port &&
            address_is_ours(responder, peer.sin_addr.s_addr)) {
            continue;
        }

        mdns_handle_packet(responder, buffer, (size_t) received,
                           peer.sin_addr.s_addr, ntohs(peer.sin_port),
                           time_now);
    }
}

static void sanitize_label(MdnsConfig *config)
{
    char cleaned[64];
    size_t out = 0;

    /* 63 characters is the DNS limit for one label; keeping the host
     * shorter than the buffer leaves room for the rename suffix. */
    for (size_t i = 0; config->host[i] != 0 && out < 63; i++) {
        unsigned char c = (unsigned char) config->host[i];

        if (c >= 'A' && c <= 'Z') {
            c = (unsigned char) (c - 'A' + 'a');
        }

        int valid = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '-';

        if (!valid) {
            /* A dot or a space would silently publish a different name. */
            break;
        }

        cleaned[out++] = (char) c;
    }

    cleaned[out] = 0;

    if (out == 0) {
        snprintf(cleaned, sizeof(cleaned), "camstream");
    }

    snprintf(config->host, sizeof(config->host), "%s", cleaned);
}

MdnsResponder *mdns_responder_create(const MdnsConfig *config,
                                     char *error,
                                     size_t error_size)
{
    if (config == NULL) {
        return NULL;
    }

    MdnsResponder *responder = calloc(1, sizeof(*responder));

    if (responder == NULL) {
        snprintf(error, error_size, "out of memory");
        return NULL;
    }

    responder->config = *config;
    responder->config.send = config->send;
    responder->config.send_user = config->send_user;

    sanitize_label(&responder->config);

    if (responder->config.model[0] == 0) {
        snprintf(responder->config.model, sizeof(responder->config.model),
                 "camstream");
    }

    snprintf(responder->label, sizeof(responder->label), "%s",
             responder->config.host);

    responder->fd = -1;
    responder->state = MDNS_PROBING;
    responder->probes_left = MDNS_PROBE_COUNT;
    responder->random_state = (uint32_t) (now_ms() ^ (uint64_t) getpid());
    responder->next_scan_ms = 0;

    snprintf(responder->stats.name, sizeof(responder->stats.name), "%s.local",
             responder->label);

    if (scan_addresses(responder) == 0 && responder->address_count == 0) {
        log_warn("mdns", "no non-loopback IPv4 address yet; announcing starts "
                         "when one appears");
    }

    if (responder->config.port != 0) {
        responder->fd = socket(AF_INET, SOCK_DGRAM, 0);

        if (responder->fd < 0) {
            snprintf(error, error_size, "socket failed: errno=%d (%s)", errno,
                     strerror(errno));
            free(responder);
            return NULL;
        }

        int reuse = 1;

        setsockopt(responder->fd, SOL_SOCKET, SO_REUSEADDR, &reuse,
                   sizeof(reuse));

#ifdef SO_REUSEPORT
        /* Shares port 5353 with avahi or mDNSResponder when one runs on
         * the same host; without it the bind below would fail. */
        setsockopt(responder->fd, SOL_SOCKET, SO_REUSEPORT, &reuse,
                   sizeof(reuse));
#endif

        struct sockaddr_in bind_address;

        memset(&bind_address, 0, sizeof(bind_address));
        bind_address.sin_family = AF_INET;
        bind_address.sin_port = htons(responder->config.port);
        bind_address.sin_addr.s_addr = htonl(INADDR_ANY);

        if (bind(responder->fd, (struct sockaddr *) &bind_address,
                 sizeof(bind_address)) != 0) {
            snprintf(error, error_size,
                     "bind to 0.0.0.0:%u failed: errno=%d (%s)",
                     responder->config.port, errno, strerror(errno));
            close(responder->fd);
            free(responder);
            return NULL;
        }

        /*
         * 255 is the required TTL for mDNS, and loopback off keeps this
         * responder from reading its own announcements back.
         */
        int ttl = 255;
        int loop = 0;

        setsockopt(responder->fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl,
                   sizeof(ttl));
        setsockopt(responder->fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop,
                   sizeof(loop));

        for (size_t i = 0; i < responder->address_count; i++) {
            set_membership(responder, responder->addresses[i], 1);
        }
    }

    responder->next_event_ms = now_ms() + MDNS_PROBE_INTERVAL_MS;

    return responder;
}

void mdns_responder_destroy(MdnsResponder *responder)
{
    if (responder == NULL) {
        return;
    }

    send_goodbye(responder);

    if (responder->fd >= 0) {
        close(responder->fd);
    }

    free(responder);
}

int mdns_fd(const MdnsResponder *responder)
{
    return responder != NULL ? responder->fd : -1;
}

const char *mdns_responder_name(const MdnsResponder *responder)
{
    return responder != NULL ? responder->stats.name : "";
}

void mdns_responder_get_stats(const MdnsResponder *responder, MdnsStats *stats)
{
    if (responder == NULL || stats == NULL) {
        return;
    }

    *stats = responder->stats;

    snprintf(stats->name, sizeof(stats->name), "%s.local", responder->label);

    for (size_t i = 0; i < responder->address_count &&
                       i < MDNS_MAX_ADDRESSES; i++) {
        memcpy(stats->addresses[i], responder->addresses[i],
               INET_ADDRSTRLEN);
    }
}
