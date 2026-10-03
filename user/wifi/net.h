#ifndef RVUOS_WIFI_NET_H
#define RVUOS_WIFI_NET_H

/*
 * A small IPv4 stack over Ethernet frames: ARP, ICMP echo, UDP and a DHCP client.
 * It makes no system call and keeps no global, so it runs in any process and on the host for its tests:
 * frames come in through net_input, time through net_tick, and frames go out through the send function it is given.
 * Addresses are kept in network order, as they travel.
 */

#include <stdint.h>

#define NET_FRAME_MAX 1514u
#define NET_UDP_MAX   1472u /* the most bytes of a datagram, in one frame */
#define NET_ARP_ENTRIES 8u
#define NET_UDP_PORTS 8u

struct net;
typedef void (*net_send_fn)(struct net *n, const uint8_t *frame, uint32_t len);

/* A datagram for a bound port: who sent it, to which of the ports, and its bytes. */
struct net_datagram {
    uint32_t from_ip;
    uint16_t from_port, port;
    const uint8_t *data;
    uint32_t len;
};
typedef void (*net_udp_fn)(struct net *n, void *arg, const struct net_datagram *d);

enum net_dhcp_state {
    DHCP_OFF,
    DHCP_SELECTING, /* a discover sent, waiting for an offer */
    DHCP_REQUESTING, /* an offer taken, a request sent, waiting for the acknowledgement */
    DHCP_BOUND,
};

struct net_arp {
    uint32_t ip;
    uint8_t mac[6];
    uint32_t used; /* when, in ms, for the oldest to go first */
};

struct net_udp {
    uint16_t port; /* in host order, 0 for a free entry */
    net_udp_fn fn;
    void *arg;     /* what fn is handed with each datagram */
};

struct net {
    net_send_fn send;
    void *owner;
    uint8_t mac[6];
    uint32_t ip, mask, gateway, dns, server;
    uint32_t now;          /* ms, as the last net_tick said */
    uint32_t dhcp_state, dhcp_xid, dhcp_offer, dhcp_sent, dhcp_tries, lease;
    uint16_t ip_id;
    struct net_arp arp[NET_ARP_ENTRIES];
    struct net_udp udp[NET_UDP_PORTS];
    uint32_t rx_frames, tx_frames, rx_dropped, pings;
    uint8_t out[NET_FRAME_MAX];
};

void net_init(struct net *n, const uint8_t mac[6], net_send_fn send, void *owner);
/* A frame from the link. */
void net_input(struct net *n, const uint8_t *frame, uint32_t len);
/* Time has moved to now ms: retransmits, and the lease. */
void net_tick(struct net *n, uint32_t now);
/* Starts DHCP: a discover now, again every two seconds until bound. */
void net_dhcp_start(struct net *n);

/* Datagrams to port, which is not 0 and not bound yet, go to fn with arg: 0, or -1 if not. */
int net_udp_bind(struct net *n, uint16_t port, net_udp_fn fn, void *arg);
void net_udp_unbind(struct net *n, uint16_t port);
/* Whether port is bound, or DHCP's, which the stack holds itself. */
int net_udp_bound(const struct net *n, uint16_t port);
/* Sends a datagram; 0 if it went, -1 if the next hop's address is not known yet, an ARP request gone instead. */
int net_udp_send(struct net *n, uint32_t to_ip, uint16_t to_port, uint16_t from_port, const uint8_t *data, uint32_t len);

/* An address in network order from its four numbers. */
uint32_t net_ip(uint32_t a, uint32_t b, uint32_t c, uint32_t d);

#endif
