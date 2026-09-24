/*
 * test_mdns.c
 *
 * Tests the mDNS responder against packets built here by hand, so every
 * check reads the bytes the responder would put on the wire.
 *
 * The responder is created with port 0 (no socket) and a capture hook,
 * which makes the test deterministic: outgoing packets are inspected in
 * memory instead of racing a network, and the clock is passed in
 * explicitly, so probing and announcement timing is checked exactly.
 *
 * The decoder in this file is deliberately strict: it refuses
 * compression pointers, which is also the assertion that the responder
 * never compresses a name inside a packet it sends.
 */
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mdns.h"

#define CAPTURE_MAX 16
#define PACKET_MAX 1500
#define RECORD_MAX 16

static int g_failures;

static void check(int condition, const char *name)
{
    printf("  %s %s\n", condition ? "ok  " : "FAIL", name);

    if (!condition) {
        g_failures++;
    }
}

static void note(const char *format, ...)
{
    va_list args;

    printf("       ");
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Capture hook                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t data[PACKET_MAX];
    size_t length;
    int multicast;              /* 1 when the destination was the group */
    struct sockaddr_in peer;    /* meaningful when multicast is 0 */
} Captured;

static Captured g_captured[CAPTURE_MAX];
static int g_capture_count;

static void capture_send(void *user,
                         const uint8_t *data,
                         size_t length,
                         const struct sockaddr_in *peer)
{
    (void) user;

    if (g_capture_count >= CAPTURE_MAX || length > PACKET_MAX) {
        return;
    }

    Captured *slot = &g_captured[g_capture_count++];

    memset(slot, 0, sizeof(*slot));
    memcpy(slot->data, data, length);
    slot->length = length;

    if (peer == NULL) {
        slot->multicast = 1;
    } else {
        slot->peer = *peer;
    }
}

static void reset_capture(void)
{
    g_capture_count = 0;
    memset(g_captured, 0, sizeof(g_captured));
}

/* ------------------------------------------------------------------ */
/* Packet building                                                     */
/* ------------------------------------------------------------------ */

static size_t write_text_name(uint8_t *out, size_t offset, const char *name)
{
    const char *label = name;

    while (*label != 0) {
        const char *dot = strchr(label, '.');
        size_t length = dot != NULL ? (size_t) (dot - label) : strlen(label);

        out[offset++] = (uint8_t) length;
        memcpy(out + offset, label, length);
        offset += length;

        if (dot == NULL) {
            break;
        }

        label = dot + 1;
    }

    out[offset++] = 0;

    return offset;
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

/* Question with an optional unicast-response bit in the class field. */
static size_t build_query(uint8_t *out,
                          uint16_t id,
                          const char *name,
                          uint16_t qtype,
                          int unicast_response)
{
    memset(out, 0, 12);
    write_be16(out + 0, id);
    write_be16(out + 4, 1);                 /* one question */

    size_t offset = write_text_name(out, 12, name);

    write_be16(out + offset, qtype);
    write_be16(out + offset + 2,
               (uint16_t) (unicast_response ? 0x8001 : 0x0001));

    return offset + 4;
}

/* Appends an A record to an existing packet and returns the new length. */
static size_t append_a_record(uint8_t *out,
                              size_t offset,
                              const char *name,
                              uint16_t klass,
                              uint32_t ttl,
                              const char *ip)
{
    offset = write_text_name(out, offset, name);
    write_be16(out + offset, 1);            /* type A */
    write_be16(out + offset + 2, klass);
    write_be32(out + offset + 4, ttl);
    write_be16(out + offset + 8, 4);

    struct in_addr addr;

    inet_pton(AF_INET, ip, &addr);
    memcpy(out + offset + 10, &addr.s_addr, 4);

    return offset + 14;
}

/* A response claiming a name, used for conflict detection checks. */
static size_t build_a_response(uint8_t *out,
                               const char *name,
                               const char *ip,
                               uint32_t ttl)
{
    memset(out, 0, 12);
    write_be16(out + 2, 0x8400);            /* QR=1, AA=1 */
    write_be16(out + 6, 1);                 /* one answer */

    return append_a_record(out, 12, name, 0x8001, ttl, ip);
}

/* ------------------------------------------------------------------ */
/* Packet decoding (strict: no compression pointers)                   */
/* ------------------------------------------------------------------ */

typedef struct {
    char name[128];
    uint16_t type;
    uint16_t klass;
    uint32_t ttl;
    size_t rdata_offset;
    uint16_t rdata_length;
} Record;

static int decode_name(const uint8_t *packet,
                       size_t length,
                       size_t offset,
                       char *out,
                       size_t out_size,
                       size_t *next_offset)
{
    size_t position = offset;
    size_t used = 0;

    for (;;) {
        if (position >= length) {
            return -1;
        }

        uint8_t label_length = packet[position];

        if (label_length == 0) {
            out[used] = 0;
            *next_offset = position + 1;
            return 0;
        }

        /* Any pointer means the responder compressed a name. */
        if ((label_length & 0xC0) != 0 || label_length > 63 ||
            position + 1 + label_length > length) {
            return -1;
        }

        if (used + (used > 0 ? 1 : 0) + label_length + 1 > out_size) {
            return -1;
        }

        if (used > 0) {
            out[used++] = '.';
        }

        memcpy(out + used, packet + position + 1, label_length);
        used += label_length;
        position += 1 + label_length;
    }
}

/* Returns 0 on a clean packet, -1 on anything malformed. */
static int decode_response(const uint8_t *packet,
                           size_t length,
                           uint16_t *id,
                           uint16_t *flags,
                           Record *records,
                           size_t max_records,
                           size_t *record_count)
{
    if (length < 12) {
        return -1;
    }

    *id = (uint16_t) ((packet[0] << 8) | packet[1]);
    *flags = (uint16_t) ((packet[2] << 8) | packet[3]);

    uint16_t questions = (uint16_t) ((packet[4] << 8) | packet[5]);
    uint16_t answers = (uint16_t) ((packet[6] << 8) | packet[7]);
    uint16_t authority = (uint16_t) ((packet[8] << 8) | packet[9]);
    uint16_t additional = (uint16_t) ((packet[10] << 8) | packet[11]);

    size_t offset = 12;

    for (uint16_t i = 0; i < questions; i++) {
        char name[128];
        size_t next = 0;

        if (decode_name(packet, length, offset, name, sizeof(name),
                        &next) != 0 || next + 4 > length) {
            return -1;
        }

        offset = next + 4;
    }

    size_t count = 0;

    for (uint32_t i = 0; i < (uint32_t) answers + authority + additional; i++) {
        if (count >= max_records) {
            return -1;
        }

        char name[128];
        size_t next = 0;

        if (decode_name(packet, length, offset, name, sizeof(name),
                        &next) != 0 || next + 10 > length) {
            return -1;
        }

        Record *record = &records[count];

        memset(record, 0, sizeof(*record));
        snprintf(record->name, sizeof(record->name), "%s", name);
        record->type = (uint16_t) ((packet[next] << 8) | packet[next + 1]);
        record->klass = (uint16_t) ((packet[next + 2] << 8) | packet[next + 3]);
        record->ttl = ((uint32_t) packet[next + 4] << 24) |
                      ((uint32_t) packet[next + 5] << 16) |
                      ((uint32_t) packet[next + 6] << 8) |
                      (uint32_t) packet[next + 7];
        record->rdata_length = (uint16_t) ((packet[next + 8] << 8) |
                                           packet[next + 9]);
        record->rdata_offset = next + 10;

        if (record->rdata_offset + record->rdata_length > length) {
            return -1;
        }

        offset = record->rdata_offset + record->rdata_length;
        count++;
    }

    *record_count = count;

    return 0;
}

static const Record *find_record(const Record *records,
                                 size_t count,
                                 const char *name,
                                 uint16_t type)
{
    for (size_t i = 0; i < count; i++) {
        if (records[i].type == type && strcmp(records[i].name, name) == 0) {
            return &records[i];
        }
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/* Test setup                                                          */
/* ------------------------------------------------------------------ */

#define HTTP_PORT_TEST 18997
#define MDNS_HOST "camstream"

static uint64_t g_now;

/*
 * The responder schedules its first probe from the real monotonic clock,
 * so the synthetic clock this test passes in has to start ahead of it.
 * Otherwise every tick would look "too early" and nothing would be sent.
 */
static uint64_t monotonic_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t) ts.tv_sec * 1000ULL + (uint64_t) ts.tv_nsec / 1000000ULL;
}

static void settle(MdnsResponder *responder);

static MdnsResponder *make_responder(const char *host)
{
    MdnsConfig config;

    memset(&config, 0, sizeof(config));
    snprintf(config.host, sizeof(config.host), "%s", host);
    snprintf(config.model, sizeof(config.model), "camstream 2.1.0");
    config.port = 0;                    /* no socket: capture only */
    config.http_port = HTTP_PORT_TEST;
    config.send = capture_send;

    char error[128] = "";

    g_now = monotonic_ms() + 1000;

    MdnsResponder *responder = mdns_responder_create(&config, error,
                                                     sizeof(error));

    if (responder == NULL) {
        printf("test_mdns: create failed: %s\n", error);
        exit(1);
    }

    /*
     * One event per service call is how the real main loop runs it, so
     * the responder is stepped until its probes and both announcements
     * are done. Otherwise the background traffic would land inside the
     * capture of a test that only cares about answers.
     */
    settle(responder);

    return responder;
}

/* Advances the clock in small steps, then forgets everything sent. */
static void settle(MdnsResponder *responder)
{
    uint64_t start = g_now;

    for (uint64_t elapsed = 100; elapsed <= 4000; elapsed += 100) {
        g_now = start + elapsed;
        mdns_service(responder, g_now);
    }

    reset_capture();
}

static void advance(MdnsResponder *responder, uint64_t milliseconds)
{
    g_now += milliseconds;
    mdns_service(responder, g_now);
}

static uint32_t peer_address(void)
{
    struct in_addr addr;

    inet_pton(AF_INET, "192.0.2.7", &addr);

    return addr.s_addr;
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

static void test_announcement(void)
{
    reset_capture();

    /* A fresh responder probes three times, then announces twice. */
    char error[128] = "";
    MdnsConfig config;

    memset(&config, 0, sizeof(config));
    snprintf(config.host, sizeof(config.host), "%s", MDNS_HOST);
    snprintf(config.model, sizeof(config.model), "camstream 2.1.0");
    config.port = 0;
    config.http_port = HTTP_PORT_TEST;
    config.send = capture_send;

    g_now = monotonic_ms() + 1000;

    uint64_t start = g_now;

    MdnsResponder *fresh = mdns_responder_create(&config, error,
                                                 sizeof(error));

    if (fresh == NULL) {
        check(0, "second responder could be created");
        return;
    }

    for (uint64_t elapsed = 0; elapsed <= 4000; elapsed += 100) {
        mdns_service(fresh, start + elapsed);
    }

    check(g_capture_count == 5,
          "three probes and two announcements are sent");
    note("packets sent in the first four seconds: %d", g_capture_count);

    Record records[RECORD_MAX];
    size_t count = 0;
    uint16_t id = 0;
    uint16_t flags = 0;

    int ok = g_capture_count > 0 &&
             decode_response(g_captured[0].data, g_captured[0].length, &id,
                             &flags, records, RECORD_MAX, &count) == 0;

    /* A probe is a query: QR clear, records in the authority section. */
    check(ok && (flags & 0x8000) == 0,
          "the first packet is a probe query, not a response");
    check(ok && count > 0, "the probe carries the records it wants to claim");
    check(g_capture_count == 5 && g_captured[4].multicast == 1 &&
              (g_captured[4].data[2] & 0x80) != 0,
          "the last packet is an unsolicited response, not a probe");

    mdns_responder_destroy(fresh);
    reset_capture();
}

static void test_multicast_query(MdnsResponder *responder)
{
    uint8_t query[512];
    size_t length = build_query(query, 0x4321, "camstream.local", 1, 0);

    reset_capture();
    mdns_handle_packet(responder, query, length, peer_address(), 5353, g_now);

    check(g_capture_count == 0,
          "a multicast answer is delayed, not sent inside the callback");

    advance(responder, 200);

    check(g_capture_count == 1, "the multicast answer is sent after the delay");
    check(g_capture_count == 1 && g_captured[0].multicast == 1,
          "the answer went to the multicast group");

    if (g_capture_count != 1) {
        return;
    }

    Record records[RECORD_MAX];
    size_t count = 0;
    uint16_t id = 0;
    uint16_t flags = 0;

    if (decode_response(g_captured[0].data, g_captured[0].length, &id, &flags,
                        records, RECORD_MAX, &count) != 0) {
        check(0, "the answer parses");
        return;
    }

    MdnsStats stats;

    mdns_responder_get_stats(responder, &stats);

    check((flags & 0x8400) == 0x8400, "QR and AA are set on a response");
    check(id == 0, "a multicast response carries no transaction ID");
    check(count == stats.address_count,
          "one A record per local address is published");

    const Record *a = find_record(records, count, "camstream.local", 1);

    check(a != NULL, "an A record for camstream.local is present");

    if (a != NULL) {
        check((a->klass & 0x8000) != 0,
              "the A record asks resolvers to flush the cache");
        check(a->ttl == 120, "the A record TTL is the mDNS default for "
                             "unique records");
        check(a->rdata_length == 4, "the A record holds one IPv4 address");

        char text[INET_ADDRSTRLEN] = "";
        struct in_addr addr;

        memcpy(&addr, g_captured[0].data + a->rdata_offset, 4);
        inet_ntop(AF_INET, &addr, text, sizeof(text));

        check(stats.address_count > 0 &&
                  strcmp(text, stats.addresses[0]) == 0,
              "the published address is a local interface address");
        note("published address %s", text);
    }
}

static void test_legacy_unicast_query(MdnsResponder *responder)
{
    uint8_t query[512];
    size_t length = build_query(query, 0x7788, "camstream.local", 1, 0);

    reset_capture();
    mdns_handle_packet(responder, query, length, peer_address(), 40000, g_now);

    check(g_capture_count == 1,
          "a legacy unicast query is answered immediately");

    if (g_capture_count != 1) {
        return;
    }

    check(g_captured[0].multicast == 0 &&
              ntohs(g_captured[0].peer.sin_port) == 40000 &&
              g_captured[0].peer.sin_addr.s_addr == peer_address(),
          "the answer goes back to the address the query came from");

    Record records[RECORD_MAX];
    size_t count = 0;
    uint16_t id = 0;
    uint16_t flags = 0;

    if (decode_response(g_captured[0].data, g_captured[0].length, &id, &flags,
                        records, RECORD_MAX, &count) != 0) {
        check(0, "the unicast answer parses");
        return;
    }

    check(id == 0x7788, "the transaction ID is echoed to a legacy resolver");
    check(count == 1 && records[0].ttl == 10,
          "a legacy answer is clamped to a 10 second TTL");
}

static void test_service_discovery(MdnsResponder *responder)
{
    uint8_t query[512];
    size_t length = build_query(query, 1, "_http._tcp.local", 12, 0);

    reset_capture();
    mdns_handle_packet(responder, query, length, peer_address(), 5353, g_now);
    advance(responder, 200);

    check(g_capture_count == 1, "a service browse is answered");

    if (g_capture_count != 1) {
        return;
    }

    Record records[RECORD_MAX];
    size_t count = 0;
    uint16_t id = 0;
    uint16_t flags = 0;

    if (decode_response(g_captured[0].data, g_captured[0].length, &id, &flags,
                        records, RECORD_MAX, &count) != 0) {
        check(0, "the service answer parses");
        return;
    }

    const Record *ptr = find_record(records, count, "_http._tcp.local", 12);

    check(ptr != NULL, "the PTR record for _http._tcp.local is present");
    check(ptr != NULL && (ptr->klass & 0x8000) == 0,
          "a shared PTR record has no cache flush bit");
    check(ptr != NULL && ptr->ttl == 4500, "the PTR TTL is 75 minutes");

    if (ptr != NULL) {
        char target[128];
        size_t next = 0;

        if (decode_name(g_captured[0].data, g_captured[0].length,
                        ptr->rdata_offset, target, sizeof(target),
                        &next) == 0) {
            check(strcmp(target, "camstream._http._tcp.local") == 0,
                  "the PTR points at the service instance");
        } else {
            check(0, "the PTR target name parses");
        }
    }

    const Record *srv = find_record(records, count,
                                    "camstream._http._tcp.local", 33);

    check(srv != NULL, "the SRV record rides along with the browse answer");

    if (srv != NULL) {
        uint16_t port = (uint16_t) ((g_captured[0].data[srv->rdata_offset + 4]
                                     << 8) |
                                    g_captured[0].data[srv->rdata_offset + 5]);

        check(port == HTTP_PORT_TEST,
              "the SRV record carries the HTTP port");
    }

    const Record *txt = find_record(records, count,
                                    "camstream._http._tcp.local", 16);

    check(txt != NULL, "the TXT record rides along too");

    if (txt != NULL) {
        char text[256];
        size_t used = 0;
        size_t position = txt->rdata_offset;
        size_t end = txt->rdata_offset + txt->rdata_length;

        while (position < end && used + 1 < sizeof(text)) {
            uint8_t entry = g_captured[0].data[position];

            if (position + 1 + entry > end) {
                break;
            }

            memcpy(text + used, g_captured[0].data + position + 1, entry);
            used += entry;
            text[used++] = ' ';
            position += 1 + entry;
        }

        if (used > 0) {
            text[used - 1] = 0;
        } else {
            text[0] = 0;
        }

        check(strstr(text, "model=camstream") != NULL,
              "the TXT record names the model");
        check(strstr(text, "path=/") != NULL,
              "the TXT record names the web path");
        note("txt: %s", text);
    }

    /* Browsing for service types is what an mDNS browser starts with. */
    length = build_query(query, 2, "_services._dns-sd._udp.local", 12, 0);

    reset_capture();
    mdns_handle_packet(responder, query, length, peer_address(), 5353, g_now);
    advance(responder, 200);

    check(g_capture_count == 1, "a service type browse is answered");

    if (g_capture_count == 1 &&
        decode_response(g_captured[0].data, g_captured[0].length, &id, &flags,
                        records, RECORD_MAX, &count) == 0) {
        const Record *types = find_record(records, count,
                                          "_services._dns-sd._udp.local", 12);

        check(types != NULL, "the service type list is returned");

        if (types != NULL) {
            char target[128];
            size_t next = 0;

            check(decode_name(g_captured[0].data, g_captured[0].length,
                              types->rdata_offset, target, sizeof(target),
                              &next) == 0 &&
                      strcmp(target, "_http._tcp.local") == 0,
                  "the service type list names _http._tcp");
        }
    }
}

static void test_unrelated_queries(MdnsResponder *responder)
{
    MdnsStats before;

    mdns_responder_get_stats(responder, &before);

    /* Another host's name. */
    uint8_t query[512];
    size_t length = build_query(query, 3, "otherhost.local", 1, 0);

    reset_capture();
    mdns_handle_packet(responder, query, length, peer_address(), 5353, g_now);
    advance(responder, 300);

    check(g_capture_count == 0, "a query for another name is not answered");

    /* Our name, but a record type we do not publish. */
    length = build_query(query, 4, "camstream.local", 28, 0);   /* AAAA */

    reset_capture();
    mdns_handle_packet(responder, query, length, peer_address(), 5353, g_now);
    advance(responder, 300);

    check(g_capture_count == 0,
          "a query for a record type we do not publish is not answered");

    /* A class we do not serve. */
    length = build_query(query, 5, "camstream.local", 1, 0);
    query[length - 1] = 3;      /* class CHAOS */

    reset_capture();
    mdns_handle_packet(responder, query, length, peer_address(), 5353, g_now);
    advance(responder, 300);

    check(g_capture_count == 0, "a query outside the IN class is not answered");

    MdnsStats after;

    mdns_responder_get_stats(responder, &after);

    check(after.responses == before.responses,
          "no response counter moved for unanswerable queries");
}

static void test_known_answer_suppression(MdnsResponder *responder)
{
    MdnsStats stats;

    mdns_responder_get_stats(responder, &stats);

    uint8_t query[512];

    memset(query, 0, sizeof(query));
    write_be16(query + 0, 6);
    write_be16(query + 4, 1);               /* one question */
    write_be16(query + 6, 1);               /* one known answer */

    size_t length = write_text_name(query, 12, "camstream.local");

    write_be16(query + length, 1);
    write_be16(query + length + 2, 1);
    length += 4;

    /* The querier already holds every address we publish. */
    length = append_a_record(query, length, "camstream.local", 1, 120,
                             stats.addresses[0]);

    reset_capture();
    mdns_handle_packet(responder, query, length, peer_address(), 5353, g_now);
    advance(responder, 300);

    if (stats.address_count == 1) {
        check(g_capture_count == 0,
              "a query that already knows the answer is not answered");
    } else {
        note("more than one local address: suppression needs all of them");
    }

    /* The same answer with a short remaining TTL must be repeated. */
    uint8_t short_ttl[512];

    memcpy(short_ttl, query, length);
    /* The record was appended as: name, type, class, TTL, length, address. */
    write_be32(short_ttl + length - 10, 10);

    reset_capture();
    mdns_handle_packet(responder, short_ttl, length, peer_address(), 5353,
                       g_now);
    advance(responder, 300);

    check(g_capture_count == 1,
          "an answer close to expiry is sent again");
}

static void test_malformed_packets(MdnsResponder *responder)
{
    MdnsStats before;

    mdns_responder_get_stats(responder, &before);

    uint8_t packet[512];
    size_t length = build_query(packet, 7, "camstream.local", 1, 0);

    reset_capture();

    /* Header only, no room for the question it claims. */
    mdns_handle_packet(responder, packet, 12, peer_address(), 5353, g_now);

    /* Truncated mid name. */
    mdns_handle_packet(responder, packet, 15, peer_address(), 5353, g_now);

    /* A label that runs past the end of the packet. */
    uint8_t bad_label[64];

    memset(bad_label, 0, sizeof(bad_label));
    write_be16(bad_label + 4, 1);
    bad_label[12] = 60;
    mdns_handle_packet(responder, bad_label, 20, peer_address(), 5353, g_now);

    /* A compression pointer that points at itself, forever. */
    uint8_t loop[64];

    memset(loop, 0, sizeof(loop));
    write_be16(loop + 4, 1);
    loop[12] = 0xC0;
    loop[13] = 12;
    mdns_handle_packet(responder, loop, 20, peer_address(), 5353, g_now);

    /* More questions than the responder is willing to walk. */
    uint8_t many[64];

    memset(many, 0, sizeof(many));
    write_be16(many + 4, 500);
    mdns_handle_packet(responder, many, 20, peer_address(), 5353, g_now);

    /* An opcode that is not QUERY. */
    memcpy(many, packet, length);
    write_be16(many + 2, 0x2800);
    mdns_handle_packet(responder, many, length, peer_address(), 5353, g_now);

    advance(responder, 300);

    MdnsStats after;

    mdns_responder_get_stats(responder, &after);

    check(g_capture_count == 0, "no malformed packet produced a response");
    check(after.parse_errors >= before.parse_errors + 6,
          "every malformed packet was counted as a parse error");
    note("parse errors: %llu -> %llu",
         (unsigned long long) before.parse_errors,
         (unsigned long long) after.parse_errors);
}

static void test_conflict_rename(void)
{
    MdnsResponder *responder = make_responder(MDNS_HOST);
    MdnsStats stats;

    mdns_responder_get_stats(responder, &stats);

    uint8_t packet[512];

    /* Another host claims our name with a different address. */
    size_t length = build_a_response(packet, "camstream.local", "198.51.100.9",
                                     120);

    mdns_handle_packet(responder, packet, length, peer_address(), 5353, g_now);

    mdns_responder_get_stats(responder, &stats);

    check(stats.conflicts == 1, "a conflicting A record is counted");
    check(stats.renamed == 1, "the conflict renames the responder");
    check(strcmp(stats.name, "camstream-2.local") == 0,
          "the new name carries a numeric suffix");
    note("renamed to %s", stats.name);

    /*
     * A second conflicting packet straight away is counted but must not
     * walk the name on: a peer that repeats its answer would otherwise
     * rename this responder until it runs out of suffixes.
     */
    length = build_a_response(packet, "camstream-2.local", "198.51.100.10",
                              120);
    mdns_handle_packet(responder, packet, length, peer_address(), 5353, g_now);

    mdns_responder_get_stats(responder, &stats);

    check(strcmp(stats.name, "camstream-2.local") == 0,
          "conflicts inside the rate limit window do not rename again");
    check(stats.conflicts == 2, "the rate limited conflict is still counted");

    /* Past the window the next conflict renames again. */
    g_now += 2000;
    mdns_handle_packet(responder, packet, length, peer_address(), 5353, g_now);

    mdns_responder_get_stats(responder, &stats);

    check(strcmp(stats.name, "camstream-3.local") == 0,
          "a later conflict renames again");
    check(stats.conflicts == 3, "each conflict is counted");

    /* Let the probing and announcements for the new name finish. */
    settle(responder);

    uint8_t query[512];
    size_t query_length = build_query(query, 8, "camstream.local", 1, 0);

    reset_capture();
    mdns_handle_packet(responder, query, query_length, peer_address(), 5353,
                       g_now);
    advance(responder, 300);

    check(g_capture_count == 0, "the abandoned name is no longer answered");

    query_length = build_query(query, 9, "camstream-3.local", 1, 0);

    reset_capture();
    mdns_handle_packet(responder, query, query_length, peer_address(), 5353,
                       g_now);
    advance(responder, 300);

    check(g_capture_count == 1, "the current name is answered");

    /* Our own data arriving from a cache is not a conflict (RFC 6762 s9). */
    uint64_t conflicts_before = stats.conflicts;

    length = build_a_response(packet, "camstream-3.local", stats.addresses[0],
                              120);
    mdns_handle_packet(responder, packet, length, peer_address(), 5353, g_now);

    mdns_responder_get_stats(responder, &stats);

    check(stats.conflicts == conflicts_before,
          "a record identical to ours is not a conflict");

    mdns_responder_destroy(responder);
}

static void test_goodbye(void)
{
    MdnsResponder *responder = make_responder(MDNS_HOST);

    reset_capture();
    mdns_responder_destroy(responder);

    check(g_capture_count > 0, "shutdown sends a goodbye packet");

    Record records[RECORD_MAX];
    size_t count = 0;
    uint16_t id = 0;
    uint16_t flags = 0;

    if (g_capture_count == 0 ||
        decode_response(g_captured[0].data, g_captured[0].length, &id, &flags,
                        records, RECORD_MAX, &count) != 0) {
        check(0, "the goodbye packet parses");
        return;
    }

    int all_zero = count > 0;

    for (size_t i = 0; i < count; i++) {
        if (records[i].ttl != 0) {
            all_zero = 0;
        }
    }

    check(all_zero, "every record in the goodbye packet has TTL 0");
}

static void test_address_count(MdnsResponder *responder)
{
    /* The responder scans the same interfaces the test can enumerate. */
    struct ifaddrs *addrs = NULL;
    size_t expected = 0;

    if (getifaddrs(&addrs) == 0) {
        for (struct ifaddrs *ifa = addrs; ifa != NULL; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == NULL ||
                ifa->ifa_addr->sa_family != AF_INET ||
                (ifa->ifa_flags & IFF_UP) == 0 ||
                (ifa->ifa_flags & IFF_LOOPBACK) != 0) {
                continue;
            }

            expected++;
        }

        freeifaddrs(addrs);
    }

    MdnsStats stats;

    mdns_responder_get_stats(responder, &stats);

    if (expected > MDNS_MAX_ADDRESSES) {
        expected = MDNS_MAX_ADDRESSES;
    }

    check(stats.address_count == expected,
          "every non-loopback IPv4 interface address is published");
    note("interfaces: %zu, published: %zu", expected, stats.address_count);
}

static void test_conflict_gives_up(void)
{
    MdnsResponder *responder = make_responder(MDNS_HOST);
    uint8_t packet[512];
    MdnsStats stats;

    /* Every name this responder tries is already taken, so it walks
     * through its suffixes until it gives up. */
    for (int i = 0; i < 12; i++) {
        mdns_responder_get_stats(responder, &stats);

        if (stats.disabled) {
            break;
        }

        size_t length = build_a_response(packet, stats.name, "198.51.100.20",
                                         120);

        g_now += 2000;
        mdns_handle_packet(responder, packet, length, peer_address(), 5353,
                           g_now);
    }

    mdns_responder_get_stats(responder, &stats);

    check(stats.disabled == 1,
          "the responder gives up when every name is taken");
    note("conflicts before giving up: %llu",
         (unsigned long long) stats.conflicts);

    /* A disabled responder answers nothing at all. */
    uint8_t query[512];
    size_t length = build_query(query, 10, "camstream.local", 1, 0);

    reset_capture();
    mdns_handle_packet(responder, query, length, peer_address(), 5353, g_now);
    advance(responder, 300);

    check(g_capture_count == 0, "a disabled responder stays silent");

    mdns_responder_destroy(responder);
}

int main(void)
{
    /* Keep the placeholder check meaningful: finding no record in an
     * empty set is how a missing record is detected elsewhere. */
    MdnsResponder *responder = make_responder(MDNS_HOST);

    printf("test_mdns: responder on %s\n", mdns_responder_name(responder));

    check(strcmp(mdns_responder_name(responder), "camstream.local") == 0,
          "the published name is <mdns_name>.local");
    check(strncmp(mdns_responder_name(NULL), "", 1) == 0,
          "a NULL responder has an empty name instead of crashing");

    test_announcement();
    test_multicast_query(responder);
    test_legacy_unicast_query(responder);
    test_service_discovery(responder);
    test_unrelated_queries(responder);
    test_known_answer_suppression(responder);
    test_malformed_packets(responder);
    test_address_count(responder);

    mdns_responder_destroy(responder);

    test_conflict_rename();
    test_conflict_gives_up();
    test_goodbye();

    if (g_failures != 0) {
        printf("test_mdns: %d check(s) failed\n", g_failures);
        return 1;
    }

    printf("test_mdns: PASS\n");

    return 0;
}
