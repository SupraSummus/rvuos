#ifndef RVUOS_WIFI_SOCK_H
#define RVUOS_WIFI_SOCK_H

/*
 * Datagram sockets over a channel: what a client in a process of its own asks of the network process,
 * and what it is answered.
 *
 * A message is a header and the data after it, one to a packet of the channel.
 * A client binds ports, each its own until it closes it or is gone, sends datagrams from the ports it holds,
 * and receives the datagrams sent to them; it may ask for the network's addresses too.
 * Every request is answered but a datagram sent, which is answered only when it could not go,
 * and answers come in the order of the requests, with the datagrams received among them.
 *
 * The server's side makes no system call and keeps no global, as net.c does, so the host tests it.
 * It trusts nothing a client writes: it reads a request's header once, since the client may change it meanwhile,
 * and keeps each client to the ports it holds.
 */

#include <stdint.h>

#include "net.h"

enum sock_op {
    SOCK_BIND = 1, /* port: the one to take, or 0 for one the server picks; answered with the port */
    SOCK_CLOSE,    /* port: one held, given back; answered */
    SOCK_SEND,     /* port: one held; ip and peer_port: where to; the data: the datagram */
    SOCK_CONFIG,   /* answered with struct sock_config as its data */
    SOCK_RECV,     /* the server's alone: a datagram to port, from ip and peer_port */
};

enum sock_status {
    SOCK_OK,
    SOCK_BAD,       /* a request the server does not know, or of another length than it says */
    SOCK_IN_USE,    /* the port is held, by this client, another, or the server itself */
    SOCK_FULL,      /* the client holds all the ports it may, or the server all it can */
    SOCK_NOT_HELD,  /* the client does not hold the port */
    SOCK_NO_ROUTE,  /* the server has no address yet, or nowhere to send to */
    SOCK_RESOLVING, /* the next hop's address is being asked for, and the datagram did not go; send it again */
};

struct sock_msg {
    uint8_t op, status;
    uint16_t port;      /* the client's */
    uint32_t ip;        /* the other side's, in network order */
    uint16_t peer_port; /* the other side's */
    uint16_t len;       /* bytes of data after the header */
};
#define SOCK_MSG_MAX (sizeof(struct sock_msg) + NET_UDP_MAX)

/* The network's addresses, in network order; ip is 0 while the network process has none. */
struct sock_config {
    uint32_t ip, mask, gateway, dns;
};

#define SOCK_CLIENTS      4u
#define SOCK_CLIENT_PORTS 2u      /* the most ports a client holds at once */
#define SOCK_EPHEMERAL    49152u  /* where the ports the server picks begin */

struct sock;
/* An answer for client: the header, and its len bytes of data, for the caller to put into the client's channel. */
typedef void (*sock_put_fn)(struct sock *s, uint32_t client, const struct sock_msg *h, const uint8_t *data);

struct sock {
    struct net *net;
    sock_put_fn put;
    void *owner;
    uint16_t ports[SOCK_CLIENTS][SOCK_CLIENT_PORTS]; /* each client's, 0 for none */
    uint16_t next_port;
    uint32_t delivered;
};

void sock_init(struct sock *s, struct net *n, sock_put_fn put, void *owner);
/* A message client put into its channel, len bytes as the ring says. */
void sock_request(struct sock *s, uint32_t client, const uint8_t *msg, uint32_t len);
/* Every port the client held, back, once it is gone. */
void sock_drop(struct sock *s, uint32_t client);

/* The client's side: a request written into slot, which holds SOCK_MSG_MAX bytes; returns its length. */
uint32_t sock_ask(uint8_t *slot, uint32_t op, uint16_t port, uint32_t ip, uint16_t peer_port, const void *data,
                  uint32_t len);
/* The client's side: an answer's header, and *data at its data; 0 if what came is not a whole message. */
int sock_answer(const uint8_t *msg, uint32_t len, struct sock_msg *h, const uint8_t **data);

#endif
