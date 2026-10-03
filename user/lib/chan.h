#ifndef RVUOS_LIB_CHAN_H
#define RVUOS_LIB_CHAN_H

/*
 * A channel between two children: a frame both have installed, holding two rings of packets, one each way,
 * and a bit each way on their inboxes.
 * Their parent makes it and connects them, and each sees the channel through an end in its page.
 *
 * An end signals the other's bit when a put makes the ring it writes non-empty,
 * and when a take makes the ring it reads non-full;
 * the other, woken by its bit, takes until its ring is empty and puts until the other is full or it has no more.
 * So one bit says both "there is something to read" and "there is room to write",
 * and an end needs to look at the bit only to know the channel is the one with news.
 */

#include <stdint.h>

#include "lib/ring.h"
#include "lib/self.h"

struct child;

/* One end, in a child's page, which its parent fills and the other child cannot write. */
struct chan_end {
    struct ring tx, rx;   /* the ring this end writes and the one it reads */
    uint32_t peer;        /* the slot of the other end's inbox, RIGHT_W */
    uint32_t peer_bit;    /* the bit the other end waits on for the channel */
    uint32_t bit;         /* the bit this end's inbox gets for it */
    uint32_t ready;       /* written last, once the rest is */
};

/* Whether the parent has connected the end yet. */
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

/* --- The parent's side. --- */

struct chan {
    struct block frame;
    struct ring a, b; /* the first half's ring and the second's */
};

/* A channel of size bytes, a power of two, its two rings of packets of up to slot_size bytes laid out. */
uint32_t chan_new(struct self *s, struct chan *ch, uint32_t size, uint32_t slot_size);
/*
 * Connects two children, which may be running: in each, the frame installed in a free region,
 * the other's inbox given in a free slot, and a bit of its inbox for the channel;
 * then the ends written into their pages, as the rings face each one, and each child told.
 */
uint32_t chan_connect(struct self *s, const struct chan *ch, struct child *a, struct chan_end *ea, struct child *b,
                      struct chan_end *eb);
/* The frame back, uninstalled from both children; the slots and bits given to them stay theirs. */
uint32_t chan_free(struct self *s, struct chan *ch);

#endif
