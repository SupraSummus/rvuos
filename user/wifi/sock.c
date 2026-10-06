/*
 * Datagram sockets over a channel; see sock.h.
 */

#include "sock.h"

#include "lib/libc.h"

void sock_init(struct sock *s, struct net *n, sock_put_fn put, void *owner)
{
    memset(s, 0, sizeof(*s));
    s->net = n;
    s->put = put;
    s->owner = owner;
    s->next_port = SOCK_EPHEMERAL;
}

/* The entry of client's that holds port, or 0; with port 0, a free entry. */
static uint16_t *held(struct sock *s, uint32_t client, uint16_t port)
{
    for (uint32_t i = 0; i < SOCK_CLIENT_PORTS; i++) {
        if (s->ports[client][i] == port) {
            return &s->ports[client][i];
        }
    }
    return 0;
}

/* A datagram for a port a client holds, into that client's channel. */
static void deliver(struct net *n, void *arg, const struct net_datagram *d)
{
    (void)n;
    struct sock *s = arg;
    for (uint32_t c = 0; c < SOCK_CLIENTS; c++) {
        if (held(s, c, d->port) != 0) {
            const struct sock_msg h = { SOCK_RECV, SOCK_OK, d->port, d->from_ip, d->from_port, (uint16_t)d->len };
            s->delivered++;
            s->put(s, c, &h, d->data);
            return;
        }
    }
}

/* A port the server picks: the next from SOCK_EPHEMERAL up that is not bound, of the few that may be. */
static uint16_t pick(struct sock *s)
{
    for (uint32_t tries = 0; tries <= NET_UDP_PORTS; tries++) {
        uint16_t port = s->next_port;
        s->next_port = port == 0xffffu ? SOCK_EPHEMERAL : (uint16_t)(port + 1u);
        if (!net_udp_bound(s->net, port)) {
            return port;
        }
    }
    return 0;
}

static uint32_t take(struct sock *s, uint32_t client, uint16_t *port)
{
    uint16_t *entry = held(s, client, 0);
    if (entry == 0) {
        return SOCK_FULL;
    }
    if (*port == 0 && (*port = pick(s)) == 0) {
        return SOCK_FULL;
    }
    if (net_udp_bound(s->net, *port)) {
        return SOCK_IN_USE;
    }
    if (net_udp_bind(s->net, *port, deliver, s) != 0) {
        return SOCK_FULL;
    }
    *entry = *port;
    return SOCK_OK;
}

static uint32_t send_from(struct sock *s, uint32_t client, const struct sock_msg *h, const uint8_t *data)
{
    if (h->port == 0 || held(s, client, h->port) == 0) {
        return SOCK_NOT_HELD;
    }
    if (s->net->ip == 0 || h->ip == 0) {
        return SOCK_NO_ROUTE;
    }
    if (net_udp_send(s->net, h->ip, h->peer_port, h->port, data, h->len) != 0) {
        return SOCK_RESOLVING;
    }
    return SOCK_OK;
}

void sock_request(struct sock *s, uint32_t client, const uint8_t *msg, uint32_t len)
{
    struct sock_msg h;
    const uint8_t *data = msg + sizeof(h);
    if (client >= SOCK_CLIENTS) {
        return;
    }
    memset(&h, 0, sizeof(h));
    if (len >= sizeof(h)) {
        memcpy(&h, msg, sizeof(h));
    }
    uint32_t status = SOCK_BAD;
    uint32_t op = h.op;
    if (len < sizeof(h) || h.len != len - sizeof(h) || h.len > NET_UDP_MAX) {
        op = 0;
    }
    switch (op) {
    case SOCK_BIND:
        status = take(s, client, &h.port);
        break;
    case SOCK_CLOSE: {
        uint16_t *entry = h.port != 0 ? held(s, client, h.port) : 0;
        status = SOCK_NOT_HELD;
        if (entry != 0) {
            net_udp_unbind(s->net, h.port);
            *entry = 0;
            status = SOCK_OK;
        }
        break;
    }
    case SOCK_SEND:
        status = send_from(s, client, &h, data);
        if (status == SOCK_OK) {
            return;
        }
        break;
    }
    h.status = (uint8_t)status;
    h.len = 0;
    s->put(s, client, &h, 0);
}

void sock_drop(struct sock *s, uint32_t client)
{
    if (client >= SOCK_CLIENTS) {
        return;
    }
    for (uint32_t i = 0; i < SOCK_CLIENT_PORTS; i++) {
        net_udp_unbind(s->net, s->ports[client][i]);
        s->ports[client][i] = 0;
    }
}

uint32_t sock_ask(uint8_t *slot, uint32_t op, uint16_t port, uint32_t ip, uint16_t peer_port, const void *data,
                  uint32_t len)
{
    const struct sock_msg h = { (uint8_t)op, SOCK_OK, port, ip, peer_port, (uint16_t)len };
    memcpy(slot, &h, sizeof(h));
    if (len != 0) {
        memcpy(slot + sizeof(h), data, len);
    }
    return sizeof(h) + len;
}

int sock_answer(const uint8_t *msg, uint32_t len, struct sock_msg *h, const uint8_t **data)
{
    if (len < sizeof(*h)) {
        return 0;
    }
    memcpy(h, msg, sizeof(*h));
    *data = msg + sizeof(*h);
    return h->len == len - sizeof(*h);
}
