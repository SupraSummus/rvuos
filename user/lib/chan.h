#ifndef RVUOS_LIB_CHAN_H
#define RVUOS_LIB_CHAN_H

/*
 * A channel between two children: a frame both have installed, holding two rings of packets, one each way,
 * and a bit each way on their inboxes.
 * Their parent makes it and connects them, and each sees the channel through an end in its page.
 *
 * Each end holds the other's inbox carved to the other's bit for the channel, so its signals say it is the channel.
 * An end signals the other when a put makes the ring it writes non-empty,
 * and when a take makes the ring it reads non-full;
 * the other, woken by its bit, takes until its ring is empty and puts until the other is full or it has no more.
 * So one bit says both "there is something to read" and "there is room to write",
 * and an end needs to look at the bit only to know the channel is the one with news.
 *
 * A peer can spoil the packets it shares and no memory past them, see lib/ring.h,
 * but it can make a ring look as full as it likes:
 * an end that serves a peer it does not trust takes a bounded number of packets for each wake, not until empty.
 *
 * A hub is a server's side of channels to many clients, below:
 * one frame of a channel for each, installed in the server in one region,
 * of which each client is given its own channel, carved, and sees no other's.
 * The parent may close a client's channel, when it takes the client down, and connect another client to it later,
 * once the server has said it let go of the last.
 */

#include <stdint.h>

#include "lib/ring.h"
#include "lib/self.h"

struct child;

/* One end, in a child's page, which its parent fills and the other child cannot write. */
struct chan_end {
    struct ring tx, rx;    /* the ring this end writes and the one it reads */
    uint32_t peer;         /* the slot of the other end's inbox, RIGHT_W, carved to its bit for the channel */
    uint32_t bit;          /* the bit this end's inbox gets for it */
    volatile uint32_t gen; /* the parent's, written last: counts each connection and close, odd while connected */
    volatile uint32_t seen; /* the child's: the last gen it has acted on, see chan_seen */
};

/* Whether the end is connected. */
int chan_ready(const struct chan_end *e);

/* The slot to fill next, or 0 if the ring is full or the end not connected. */
uint8_t *chan_put_begin(const struct chan_end *e);
/* Hands it over with len bytes, and wakes the other end if it may be waiting. */
void chan_put_end(const struct chan_end *e, uint32_t len);
/* The next packet and its length, or 0 if there is none or the end is not connected. */
const uint8_t *chan_get_begin(const struct chan_end *e, uint32_t *len);
/* Gives its slot back, and wakes the other end if it may be waiting. */
void chan_get_end(const struct chan_end *e);
/* A packet copied in and handed over: 0, or -1 if the ring is full, the packet longer than a slot or the end not connected. */
int chan_send(const struct chan_end *e, const void *data, uint32_t len);

/*
 * Whether the parent has connected or closed the end since the child last said it saw, and *gen the count now.
 * A server looks when its parent tells it something, and forgets whatever it held for the client before,
 * since a closed end's client is gone and a connected one's is new.
 */
int chan_changed(const struct chan_end *e, uint32_t *gen);
/* Says the child acted on gen and touches nothing of the end's last client, and tells the parent. */
void chan_seen(struct chan_end *e, uint32_t gen);

/* --- The parent's side. --- */

struct chan {
    struct block frame;
    struct ring a, b; /* the first half's ring and the second's */
};

/* A channel of size bytes, a power of two, its two rings of packets of up to slot_size bytes laid out. */
uint32_t chan_new(struct self *s, struct chan *ch, uint32_t size, uint32_t slot_size);
/*
 * Connects two children, which may be running: in each, the frame installed in a free region,
 * a bit of its inbox for the channel, and the other's inbox, carved to the other's bit, in a free slot;
 * then the ends written into their pages, as the rings face each one, and each child told.
 */
uint32_t chan_connect(struct self *s, const struct chan *ch, struct child *a, struct chan_end *ea, struct child *b,
                      struct chan_end *eb);
/* The frame back, uninstalled from both children; the slots and bits given to them stay theirs. */
uint32_t chan_free(struct self *s, struct chan *ch);

#define CHAN_HUB_MAX 8u /* the most clients a hub has channels for */

struct chan_hub {
    struct block frame;        /* every client's channel, one after another */
    struct child *server;
    struct chan_end *ends;     /* the server's, in its page, one for each client */
    uint32_t count, size, slot_size;
    uint32_t slices[CHAN_HUB_MAX]; /* the parent's slot of each connected client's channel, carved, or 0 */
};

/*
 * A hub of count channels of size bytes each, a power of two, in the server, which may be running:
 * the frame of them all installed in one free region of the server's, and for each end a bit of its inbox
 * and a slot it is to hold the client's inbox in; its ends are written into its page, closed.
 */
uint32_t chan_hub_new(struct self *s, struct chan_hub *h, struct child *server, struct chan_end *ends, uint32_t count,
                      uint32_t size, uint32_t slot_size);
/*
 * Connects a client to end i, both of which may be running, once the end is idle:
 * its channel carved and installed in the client, a bit of the client's inbox for it,
 * and each side's inbox in the other's table, carved to its bit; then both ends written and both children told.
 * KERR_STATE when the server has not let go of the end's last client yet.
 */
uint32_t chan_hub_connect(struct self *s, struct chan_hub *h, uint32_t i, struct child *client, struct chan_end *ce);
/*
 * Closes end i, before or after its client is taken down:
 * the channel taken from the client, the client's inbox from the server's table, and the server told,
 * which says when it has let go, chan_seen; until then the end is not idle.
 * A client that still runs faults at its next touch of the channel.
 */
uint32_t chan_hub_close(struct self *s, struct chan_hub *h, uint32_t i);
/* Whether end i is closed and the server has let go of it, so that a client may be connected to it. */
int chan_hub_idle(const struct chan_hub *h, uint32_t i);
/* The frame back, from the server and every client; the slots and bits given to the server stay its. */
uint32_t chan_hub_free(struct self *s, struct chan_hub *h);

#endif
