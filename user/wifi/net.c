/*
 * The IPv4 stack; see net.h.
 * Formats are RFC 826 (ARP), 791 (IPv4), 792 (ICMP), 768 (UDP) and 2131 with 2132 (DHCP).
 */

#include "net.h"

#include "lib.h"

#define ETH_HEADER  14u
#define IP_HEADER   20u
#define UDP_HEADER  8u
#define ETH_ARP     0x0806u
#define ETH_IP      0x0800u
#define IP_ICMP     1u
#define IP_UDP      17u
#define DHCP_CLIENT 68u
#define DHCP_SERVER 67u
#define DHCP_RETRY_MS 2000u

#define BROADCAST 0xffffffffu

static const uint8_t broadcast_mac[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static void put_be16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint32_t get_ip(const uint8_t *p)
{
    uint32_t ip;
    memcpy(&ip, p, 4);
    return ip;
}

static void put_ip(uint8_t *p, uint32_t ip)
{
    memcpy(p, &ip, 4);
}

uint32_t net_ip(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    uint8_t p[4] = { (uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d };
    return get_ip(p);
}

/* The one's complement sum of big-endian 16-bit words, folded later. */
static uint32_t sum16(uint32_t sum, const uint8_t *p, uint32_t len)
{
    for (; len > 1; len -= 2, p += 2) {
        sum += be16(p);
    }
    if (len) {
        sum += (uint32_t)p[0] << 8;
    }
    return sum;
}

static uint16_t fold(uint32_t sum)
{
    while (sum >> 16) {
        sum = (sum & 0xffffu) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

void net_init(struct net *n, const uint8_t mac[6], net_send_fn send, void *owner)
{
    memset(n, 0, sizeof(*n));
    memcpy(n->mac, mac, 6);
    n->send = send;
    n->owner = owner;
    n->ip_id = (uint16_t)((mac[4] << 8) | mac[5]);
}

static void arp_learn(struct net *n, uint32_t ip, const uint8_t *mac)
{
    if (ip == 0 || ip == BROADCAST) {
        return;
    }
    struct net_arp *slot = &n->arp[0];
    for (uint32_t i = 0; i < NET_ARP_ENTRIES; i++) {
        struct net_arp *a = &n->arp[i];
        if (a->ip == ip) {
            slot = a;
            break;
        }
        if (a->ip == 0 || a->used < slot->used) {
            slot = a;
        }
    }
    slot->ip = ip;
    memcpy(slot->mac, mac, 6);
    slot->used = n->now + 1u;
}

static const uint8_t *arp_find(struct net *n, uint32_t ip)
{
    for (uint32_t i = 0; i < NET_ARP_ENTRIES; i++) {
        if (n->arp[i].ip == ip) {
            n->arp[i].used = n->now + 1u;
            return n->arp[i].mac;
        }
    }
    return 0;
}

static void eth_send(struct net *n, const uint8_t *to, uint32_t type, uint32_t len)
{
    memcpy(n->out, to, 6);
    memcpy(n->out + 6, n->mac, 6);
    put_be16(n->out + 12, type);
    n->tx_frames++;
    n->send(n, n->out, ETH_HEADER + len);
}

static void arp_send(struct net *n, uint32_t op, const uint8_t *to_mac, uint32_t to_ip)
{
    uint8_t *a = n->out + ETH_HEADER;
    put_be16(a, 1);
    put_be16(a + 2, ETH_IP);
    a[4] = 6;
    a[5] = 4;
    put_be16(a + 6, op);
    memcpy(a + 8, n->mac, 6);
    put_ip(a + 14, n->ip);
    memcpy(a + 18, op == 1 ? (const uint8_t[6]){ 0 } : to_mac, 6);
    put_ip(a + 24, to_ip);
    eth_send(n, op == 1 ? broadcast_mac : to_mac, ETH_ARP, 28);
}

static int on_link(const struct net *n, uint32_t ip)
{
    return n->mask != 0 && (ip & n->mask) == (n->ip & n->mask);
}

/*
 * Sends the IP packet whose payload of len bytes the caller wrote at out + 34:
 * to the address itself on the link, else to the gateway, broadcast to everyone.
 * -1 if the next hop's address is unknown, when an ARP request goes instead.
 */
static int ip_send(struct net *n, uint32_t to, uint32_t proto, uint32_t len)
{
    const uint8_t *mac;
    if (to == BROADCAST || (n->mask != 0 && to == (n->ip | ~n->mask))) {
        mac = broadcast_mac;
    } else {
        uint32_t hop = on_link(n, to) ? to : n->gateway;
        if (hop == 0) {
            return -1;
        }
        mac = arp_find(n, hop);
        if (mac == 0) {
            arp_send(n, 1, 0, hop);
            return -1;
        }
    }
    uint8_t *ip = n->out + ETH_HEADER;
    ip[0] = 0x45;
    ip[1] = 0;
    put_be16(ip + 2, IP_HEADER + len);
    put_be16(ip + 4, n->ip_id++);
    put_be16(ip + 6, 0x4000u); /* don't fragment */
    ip[8] = 64;
    ip[9] = (uint8_t)proto;
    put_be16(ip + 10, 0);
    put_ip(ip + 12, n->ip);
    put_ip(ip + 16, to);
    put_be16(ip + 10, fold(sum16(0, ip, IP_HEADER)));
    eth_send(n, mac, ETH_IP, IP_HEADER + len);
    return 0;
}

static uint16_t udp_checksum(uint32_t from, uint32_t to, const uint8_t *udp, uint32_t len)
{
    uint8_t pseudo[12];
    put_ip(pseudo, from);
    put_ip(pseudo + 4, to);
    pseudo[8] = 0;
    pseudo[9] = IP_UDP;
    put_be16(pseudo + 10, len);
    uint16_t c = fold(sum16(sum16(0, pseudo, sizeof(pseudo)), udp, len));
    return c == 0 ? 0xffffu : c;
}

/* The UDP header for a payload of len bytes already at out + 42, then the IP packet. */
static int udp_out(struct net *n, uint32_t to, uint16_t to_port, uint16_t from_port, uint32_t len)
{
    uint8_t *u = n->out + ETH_HEADER + IP_HEADER;
    put_be16(u, from_port);
    put_be16(u + 2, to_port);
    put_be16(u + 4, UDP_HEADER + len);
    put_be16(u + 6, 0);
    put_be16(u + 6, udp_checksum(n->ip, to, u, UDP_HEADER + len));
    return ip_send(n, to, IP_UDP, UDP_HEADER + len);
}

int net_udp_send(struct net *n, uint32_t to, uint16_t to_port, uint16_t from_port, const uint8_t *data, uint32_t len)
{
    if (len > NET_FRAME_MAX - ETH_HEADER - IP_HEADER - UDP_HEADER) {
        return -1;
    }
    memmove(n->out + ETH_HEADER + IP_HEADER + UDP_HEADER, data, len);
    return udp_out(n, to, to_port, from_port, len);
}

int net_udp_bind(struct net *n, uint16_t port, net_udp_fn fn)
{
    for (uint32_t i = 0; i < NET_UDP_PORTS; i++) {
        if (n->udp[i].port == 0) {
            n->udp[i].port = port;
            n->udp[i].fn = fn;
            return 0;
        }
    }
    return -1;
}

/* DHCP: a discover, or a request for the offer taken, broadcast from 0.0.0.0. */
static void dhcp_send(struct net *n, uint32_t type)
{
    uint8_t *d = n->out + ETH_HEADER + IP_HEADER + UDP_HEADER;
    memset(d, 0, 300);
    d[0] = 1; /* a request */
    d[1] = 1; /* Ethernet */
    d[2] = 6;
    memcpy(d + 4, &n->dhcp_xid, 4);
    put_be16(d + 10, 0x8000u); /* answer by broadcast: the stack takes no unicast before it has an address */
    memcpy(d + 28, n->mac, 6);
    static const uint8_t cookie[4] = { 99, 130, 83, 99 };
    memcpy(d + 236, cookie, 4);
    uint8_t *o = d + 240;
    *o++ = 53;
    *o++ = 1;
    *o++ = (uint8_t)type;
    if (type == 3) {
        *o++ = 50;
        *o++ = 4;
        put_ip(o, n->dhcp_offer);
        o += 4;
        *o++ = 54;
        *o++ = 4;
        put_ip(o, n->server);
        o += 4;
    }
    static const uint8_t ask[] = { 55, 4, 1, 3, 6, 51, 12, 5, 'r', 'v', 'u', 'o', 's' };
    memcpy(o, ask, sizeof(ask));
    o += sizeof(ask);
    *o++ = 255;
    uint32_t len = (uint32_t)(o - d) < 300u ? 300u : (uint32_t)(o - d);
    uint32_t ip = n->ip;
    n->ip = 0;
    udp_out(n, BROADCAST, DHCP_SERVER, DHCP_CLIENT, len);
    n->ip = ip;
    n->dhcp_sent = n->now;
    n->dhcp_tries++;
}

void net_dhcp_start(struct net *n)
{
    n->ip = 0;
    n->mask = 0;
    n->gateway = 0;
    n->dhcp_xid = get_ip(n->mac + 2) ^ (n->now * 2654435761u) ^ n->dhcp_tries;
    n->dhcp_state = DHCP_SELECTING;
    dhcp_send(n, 1);
}

static void dhcp_input(struct net *n, const uint8_t *d, uint32_t len)
{
    static const uint8_t cookie[4] = { 99, 130, 83, 99 };
    if (len < 240 || d[0] != 2 || memcmp(d + 4, &n->dhcp_xid, 4) != 0 || memcmp(d + 28, n->mac, 6) != 0 ||
        memcmp(d + 236, cookie, 4) != 0) {
        return;
    }
    uint32_t type = 0, mask = 0, router = 0, dns = 0, server = 0, lease = 0;
    for (uint32_t i = 240; i < len && d[i] != 255;) {
        uint32_t opt = d[i];
        if (opt == 0) {
            i++;
            continue;
        }
        if (i + 2 > len || i + 2 + d[i + 1] > len) {
            break;
        }
        uint32_t olen = d[i + 1];
        const uint8_t *v = d + i + 2;
        if (opt == 53 && olen >= 1) {
            type = v[0];
        } else if (opt == 1 && olen >= 4) {
            mask = get_ip(v);
        } else if (opt == 3 && olen >= 4) {
            router = get_ip(v);
        } else if (opt == 6 && olen >= 4) {
            dns = get_ip(v);
        } else if (opt == 54 && olen >= 4) {
            server = get_ip(v);
        } else if (opt == 51 && olen >= 4) {
            lease = ((uint32_t)v[0] << 24) | ((uint32_t)v[1] << 16) | ((uint32_t)v[2] << 8) | v[3];
        }
        i += 2 + olen;
    }
    uint32_t yiaddr = get_ip(d + 16);
    if (n->dhcp_state == DHCP_SELECTING && type == 2 && yiaddr != 0) {
        n->dhcp_offer = yiaddr;
        n->server = server;
        n->dhcp_state = DHCP_REQUESTING;
        dhcp_send(n, 3);
    } else if (n->dhcp_state == DHCP_REQUESTING && type == 5 && yiaddr == n->dhcp_offer) {
        n->ip = yiaddr;
        n->mask = mask;
        n->gateway = router;
        n->dns = dns;
        n->lease = lease;
        n->dhcp_state = DHCP_BOUND;
        n->dhcp_sent = n->now;
    } else if (n->dhcp_state == DHCP_REQUESTING && type == 6) {
        n->dhcp_state = DHCP_SELECTING;
        n->dhcp_sent = n->now - DHCP_RETRY_MS;
    }
}

static void icmp_input(struct net *n, uint32_t from, const uint8_t *p, uint32_t len)
{
    if (len < 8 || p[0] != 8 || fold(sum16(0, p, len)) != 0) {
        return;
    }
    if (len > NET_FRAME_MAX - ETH_HEADER - IP_HEADER) {
        return;
    }
    uint8_t *r = n->out + ETH_HEADER + IP_HEADER;
    memmove(r, p, len);
    r[0] = 0; /* an echo reply */
    put_be16(r + 2, 0);
    put_be16(r + 2, fold(sum16(0, r, len)));
    n->pings++;
    ip_send(n, from, IP_ICMP, len);
}

static void udp_input(struct net *n, uint32_t from, uint32_t to, const uint8_t *u, uint32_t len)
{
    if (len < UDP_HEADER || be16(u + 4) < UDP_HEADER || be16(u + 4) > len) {
        return;
    }
    len = be16(u + 4);
    if (be16(u + 6) != 0) {
        uint8_t pseudo[12];
        put_ip(pseudo, from);
        put_ip(pseudo + 4, to);
        pseudo[8] = 0;
        pseudo[9] = IP_UDP;
        put_be16(pseudo + 10, len);
        if (fold(sum16(sum16(0, pseudo, sizeof(pseudo)), u, len)) != 0) {
            n->rx_dropped++;
            return;
        }
    }
    uint16_t port = be16(u + 2);
    if (port == DHCP_CLIENT) {
        dhcp_input(n, u + UDP_HEADER, len - UDP_HEADER);
        return;
    }
    for (uint32_t i = 0; i < NET_UDP_PORTS; i++) {
        if (n->udp[i].port == port && n->udp[i].fn) {
            n->udp[i].fn(n, from, be16(u), u + UDP_HEADER, len - UDP_HEADER);
            return;
        }
    }
}

static void ip_input(struct net *n, const uint8_t *eth, const uint8_t *p, uint32_t len)
{
    if (len < IP_HEADER || (p[0] >> 4) != 4) {
        return;
    }
    uint32_t ihl = (p[0] & 0x0fu) * 4u;
    uint32_t total = be16(p + 2);
    if (ihl < IP_HEADER || total < ihl || total > len || fold(sum16(0, p, ihl)) != 0) {
        n->rx_dropped++;
        return;
    }
    if (be16(p + 6) & 0x3fffu) {
        return; /* a fragment: nothing here reassembles */
    }
    uint32_t from = get_ip(p + 12), to = get_ip(p + 16);
    int mine = (n->ip != 0 && to == n->ip) || to == BROADCAST || (n->mask != 0 && to == (n->ip | ~n->mask));
    if (!mine && !(n->ip == 0 && p[9] == IP_UDP)) {
        return;
    }
    if (n->ip != 0 && on_link(n, from)) {
        arp_learn(n, from, eth + 6);
    }
    if (p[9] == IP_ICMP && to == n->ip) {
        icmp_input(n, from, p + ihl, total - ihl);
    } else if (p[9] == IP_UDP) {
        udp_input(n, from, to, p + ihl, total - ihl);
    }
}

static void arp_input(struct net *n, const uint8_t *a, uint32_t len)
{
    if (len < 28 || be16(a) != 1 || be16(a + 2) != ETH_IP || a[4] != 6 || a[5] != 4) {
        return;
    }
    uint32_t op = be16(a + 6), sender = get_ip(a + 14), target = get_ip(a + 24);
    if (n->ip == 0) {
        return;
    }
    if (op == 2 || target == n->ip) {
        arp_learn(n, sender, a + 8);
    }
    if (op == 1 && target == n->ip) {
        arp_send(n, 2, a + 8, sender);
    }
}

void net_input(struct net *n, const uint8_t *frame, uint32_t len)
{
    if (len < ETH_HEADER) {
        return;
    }
    if (memcmp(frame, n->mac, 6) != 0 && !(frame[0] & 1u)) {
        return; /* neither ours nor broadcast or multicast */
    }
    n->rx_frames++;
    uint32_t type = be16(frame + 12);
    if (type == ETH_ARP) {
        arp_input(n, frame + ETH_HEADER, len - ETH_HEADER);
    } else if (type == ETH_IP) {
        ip_input(n, frame, frame + ETH_HEADER, len - ETH_HEADER);
    }
}

void net_tick(struct net *n, uint32_t now)
{
    n->now = now;
    if ((n->dhcp_state == DHCP_SELECTING || n->dhcp_state == DHCP_REQUESTING) && now - n->dhcp_sent >= DHCP_RETRY_MS) {
        net_dhcp_start(n);
    } else if (n->dhcp_state == DHCP_BOUND && n->lease != 0 &&
               now - n->dhcp_sent >= (n->lease < 4000000u ? n->lease : 4000000u) * 500u) {
        net_dhcp_start(n); /* half the lease gone: begin again, which a renewal would spare */
    }
}
