/*
 * mdns.h
 *
 * Minimal multicast DNS responder (RFC 6762) with a DNS-SD HTTP service
 * registration (RFC 6763), so a browser on the same LAN reaches the
 * server by name instead of by address.
 *
 * A LAN address changes with DHCP, with the router and with which
 * interface is up, and reading it out of a log is a step nobody wants to
 * repeat after every reboot. Publishing a name removes that step: the
 * page is at http://camstream.local:8080/ from any device that speaks
 * mDNS, over a router or over a single cable between the Pi and a
 * laptop.
 *
 * Published records:
 *   camstream.local                  A    one per IPv4 address
 *   camstream._http._tcp.local       SRV  HTTP port, target camstream.local
 *   camstream._http._tcp.local       TXT  model, path
 *   _http._tcp.local                 PTR  camstream._http._tcp.local
 *   _services._dns-sd._udp.local     PTR  _http._tcp.local
 *
 * Behaviour: three probes before the first announcement so an existing
 * owner of the name is found, two announcements one second apart, then
 * answers to queries. A name conflict renames the responder
 * (camstream-2, camstream-3, ...) and probes again. The interface list
 * is rescanned, and a new address (cable plugged in later) triggers a
 * fresh announcement. Shutdown sends goodbye packets with TTL 0 so
 * resolvers drop the name instead of caching it.
 *
 * Deliberately not implemented: IPv6 AAAA records, and the full
 * unique/shared reclaim procedure of RFC 6762 section 9 (this responder
 * probes, renames and gives up; it never claims a name by force). Both
 * cost state and code without changing the use case.
 *
 * Ownership: mdns_responder_create() allocates the object and, unless
 * the port is 0, one UDP socket bound to 0.0.0.0:<port>.
 * mdns_responder_destroy() sends the goodbye packets, closes the socket
 * and frees everything, so the caller owns nothing afterwards. The
 * object belongs to a single thread: the caller must run mdns_service()
 * and mdns_readable() from the thread that owns it.
 */
#ifndef NET_MDNS_H
#define NET_MDNS_H

#include <arpa/inet.h>
#include <stddef.h>
#include <stdint.h>

#define MDNS_MAX_ADDRESSES 8
#define MDNS_NAME_MAX 128

typedef struct MdnsResponder MdnsResponder;

/*
 * Delivery hook. When set, every outgoing packet is handed to it instead
 * of the socket: peer == NULL means "to the multicast group, one call
 * per interface", otherwise a legacy unicast reply to that address.
 * Tests use it to inspect responses without touching the network.
 */
typedef void (*MdnsSendFn)(void *user,
                           const uint8_t *data,
                           size_t length,
                           const struct sockaddr_in *peer);

typedef struct {
    char host[64];          /* label only, no dots: "camstream" */
    char model[64];         /* TXT model= value, for example "camstream 2.1.0" */
    uint16_t port;          /* responder UDP port; 5353, or 0 for no socket */
    uint16_t http_port;     /* port advertised in the SRV record */
    MdnsSendFn send;        /* NULL sends on the socket */
    void *send_user;
} MdnsConfig;

typedef struct {
    uint64_t queries;       /* queries received and understood */
    uint64_t responses;     /* responses sent */
    uint64_t announcements; /* probes and unsolicited announcements sent */
    uint64_t conflicts;     /* name conflicts seen */
    uint64_t parse_errors;  /* dropped packets that did not parse */
    uint64_t socket_errors; /* send, receive and getifaddrs failures */
    size_t address_count;
    char addresses[MDNS_MAX_ADDRESSES][INET_ADDRSTRLEN];
    char name[MDNS_NAME_MAX];   /* current name, "camstream.local" */
    int renamed;                /* 1 when a conflict changed the name */
    int disabled;               /* 1 when no free name was found */
} MdnsStats;

/*
 * Create the responder. Returns NULL and fills 'error' when the socket
 * cannot be opened; a port of 0 skips the socket entirely. The name is
 * lowercased and validated, falling back to "camstream" when unusable.
 */
MdnsResponder *mdns_responder_create(const MdnsConfig *config,
                                     char *error,
                                     size_t error_size);

/* Send the goodbye packets and free the responder. NULL is accepted. */
void mdns_responder_destroy(MdnsResponder *responder);

/* Socket for the caller's poll set, or -1 when there is none. */
int mdns_fd(const MdnsResponder *responder);

/* Current name, "camstream.local". Never NULL for a live responder. */
const char *mdns_responder_name(const MdnsResponder *responder);

/*
 * Advance timers (probes, announcements, pending replies, address
 * rescan). Call once per main loop iteration with a monotonic timestamp
 * in milliseconds.
 */
void mdns_service(MdnsResponder *responder, uint64_t now_ms);

/*
 * Read and answer every datagram that is waiting on the socket. Returns
 * without blocking when there is nothing to read.
 */
void mdns_readable(MdnsResponder *responder, uint64_t now_ms);

/*
 * Handle one received packet. Exposed for tests; mdns_readable() calls
 * it for every datagram. peer_ip_be is the source address in network
 * byte order, peer_port the source port in host byte order.
 */
void mdns_handle_packet(MdnsResponder *responder,
                        const uint8_t *packet,
                        size_t length,
                        uint32_t peer_ip_be,
                        uint16_t peer_port,
                        uint64_t now_ms);

void mdns_responder_get_stats(const MdnsResponder *responder, MdnsStats *stats);

#endif
