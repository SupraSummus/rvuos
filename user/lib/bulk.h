#ifndef RVUOS_LIB_BULK_H
#define RVUOS_LIB_BULK_H

/*
 * Bulk data between processes: buffers a lender lends its borrowers where they lie.
 *
 * A lender, a driver or a server, holds each of its connections' buffers in its own memory,
 * and hands them to the connection's borrower through a ring of references;
 * the borrower hands them back through a ring of its own.
 * A reference says which buffer, where in it the bytes begin and how many, and a word the two give a meaning to;
 * the bytes pass through neither ring nor the kernel.
 * Their parent makes a lender's connections and connects a borrower to each, as it does a hub's channels, lib/chan.h;
 * each side sees its connection through an end in its page, which the other cannot write.
 *
 * A buffer to read the lender fills, or its device does, and lends with its bytes;
 * the borrower maps it read only, and gives it back once it has read them.
 * A buffer to fill the parent lends the borrower as it connects it;
 * the borrower fills it and hands it back with its bytes, and the lender gives it back once it has used them.
 * So both sides do the same four things: take a buffer to fill, bulk_take; hand it over with its bytes, bulk_hand;
 * take the next one the other side handed over, bulk_next; and give that back once its bytes are used, bulk_put.
 * A buffer handed over wakes the other side, unless a buffer it was woken for is still unread;
 * a buffer given back is room, which wakes the other side only if it asked for room, by finding none to take.
 *
 * The lender is the side its borrowers trust and that outlives them; see DESIGN.md, "Bulk data".
 * It keeps which buffers it lent where no borrower writes, and takes back only a buffer it lent,
 * so a reference a borrower forges costs nothing but that reference.
 * A borrower may read every buffer lent to read, lent to it or not yet,
 * so a lender puts into a connection's buffers only what that borrower may see.
 * A buffer lent to fill comes back with what its borrower wrote, which the borrower can change still:
 * the lender reads once, into memory of its own, what it decides on, a header,
 * and carries the rest, copying it or handing it to a device, deciding nothing on it twice.
 *
 * Neither side waits for the other:
 * it takes for each wake no more references than the other's ring held as its turn began,
 * and does without when it finds no buffer to take.
 *
 * A driver whose device reaches memory by itself, a bus master, reaches all of RAM, PMP or not,
 * and is trusted as its parent is, DESIGN.md, open decision 13:
 * it hands its device only buffers its end says are its connection's,
 * and once a connection is closed it stops its device using them before it says it let go of them, bulk_seen.
 *
 * One thread of each side uses an end at a time.
 */

#include <stdint.h>

#include "lib/chan.h"
#include "lib/self.h"

struct child;

#define BULK_BUFFERS     64u /* the most buffers of a connection, of both kinds together */
#define BULK_CONNECTIONS 8u  /* the most connections a lender has */
#define BULK_SIZE_MAX    0x100000u /* a buffer's bytes are fewer */

/*
 * A reference in a ring is a packet of three words, lib/ring.h:
 * the buffer above BULK_OFF_BITS and the offset of its bytes below them, then how many bytes, then the tag.
 * A buffer to read lent and a buffer to fill given back carry bytes; the other two are room.
 */
#define BULK_OFF_BITS  24u
#define BULK_REF_BYTES 12u

/* A buffer, as a side sees it once it holds it. */
struct bulk_ref {
    uint32_t buf;         /* which: those lent to read first, then those lent to fill */
    uint32_t off, len;    /* where in it its bytes begin, and how many */
    uint32_t tag;         /* a word the two sides give a meaning to, beside the bytes */
    const uint8_t *bytes; /* where its bytes begin */
    uint8_t *room;        /* the same if this side may write them: any of the lender's, a borrower's to fill; else 0 */
};

/* One side of a connection, in its page, which its parent fills and the other side cannot write. */
struct bulk_end {
    struct chan_end refs;          /* the ring of references this side writes and the other's, and a bit each way */
    uint32_t lender;               /* 1 on the lender's side */
    uint32_t reads, fills, size;   /* how many buffers are lent to read and how many to fill, and the bytes of each */
    uint32_t read_base, fill_base; /* where the first of each kind lies */
    uint32_t wants;                /* where the two words lie that say a side asked for room, the lender's first */
    /* This side's alone. */
    uint32_t sent, woke;               /* the references it put, and how many it had put when it last woke the other */
    uint32_t held[BULK_BUFFERS / 32];  /* the buffers it took, or was handed with bytes, a bit each */
    uint32_t away[BULK_BUFFERS / 32];  /* the lender's: those lent and not had back */
    uint32_t spare[BULK_BUFFERS / 32]; /* a borrower's: those lent to it to fill that it has not taken */
    volatile uint32_t refused;     /* references the other side handed over that named nothing it could, for the record */
};

/* Whether the end is connected. */
int bulk_ready(const struct bulk_end *e);

/*
 * A buffer this side may fill, whole: the lender's own that no one holds, to lend to read;
 * a borrower's, lent to it to fill. 0, or -1 if there is none,
 * and the side has asked for room then: the next buffer the other side gives back to it wakes it.
 */
int bulk_take(struct bulk_end *e, struct bulk_ref *r);
/*
 * Hands the other side a buffer this side took, with r->len bytes from r->off, and r->tag:
 * the lender lends it to read; a borrower gives it back filled.
 * 0, or -1 if this side did not take it;
 * or -1 if the bytes are not the buffer's or the other side cannot be handed it,
 * the end closed or its ring made to look full, and the buffer is back among those this side may take.
 */
int bulk_hand(struct bulk_end *e, const struct bulk_ref *r);

/* What a side takes of the other's ring for one wake: what it held as the turn began, a ring's worth at most. */
struct bulk_turn {
    struct bulk_end *e;
    uint32_t left;
};
struct bulk_turn bulk_turn(struct bulk_end *e);
/*
 * The next buffer the other side handed this one with bytes, read once, in *r; 1, or 0 once the turn is over.
 * The lender is given back a buffer to fill, filled; a borrower is lent a buffer to read.
 * Room the turn passes is taken in on the way, and a reference that names nothing this side could be handed
 * is passed over and counted in refused.
 */
int bulk_next(struct bulk_turn *t, struct bulk_ref *r);
/* Whether the ring held more once the turn was over, which the side takes on a wake of its own, signalling itself. */
int bulk_more(const struct bulk_turn *t);
/*
 * A buffer this side holds back where it came from: one it was handed with bytes, once it has used them,
 * given back to the other side as room, which wakes the other side only if it asked for room;
 * one it took and did not hand, back among those it may take.
 */
void bulk_put(struct bulk_end *e, uint32_t buf);

/*
 * Whether the parent has connected or closed the end since the side last said it saw, and *gen the count now.
 * The lender looks when its parent tells it something; a closed connection's buffers are all its own again,
 * and it says so with bulk_seen once nothing of its, its device least of all, touches them any more.
 */
int bulk_changed(const struct bulk_end *e, uint32_t *gen);
/* Says the side acted on gen, and tells the parent; on a close, the side holds none of the connection's buffers. */
void bulk_seen(struct bulk_end *e, uint32_t gen);

/* --- The parent's side. --- */

/* The buffers of each of a lender's connections. */
struct bulk_shape {
    uint32_t size;         /* the bytes of a buffer, a multiple of eight, less than BULK_SIZE_MAX */
    uint32_t reads, fills; /* how many are lent to read and how many to fill, BULK_BUFFERS at most together */
};

struct bulk {
    struct block frame;            /* every connection's two blocks */
    struct child *lender;
    struct bulk_end *ends;         /* the lender's, in its page, one for each connection */
    uint32_t count;
    struct bulk_shape shape;
    uint32_t read_size, both_size; /* a connection's block lent to read, 0 if it lends none so, and its block both write */
    uint32_t read_at, both_at;     /* where in the frame the first connection's of each lies; the next lie one on */
    /* Each connection's borrower, and the regions its blocks are installed in, both write and lent to read, or 0. */
    struct child *borrowers[BULK_CONNECTIONS];
    uint32_t regions[BULK_CONNECTIONS][2];
};

/*
 * A lender's count connections, each of shape, in one frame installed in one free region of the lender's,
 * which may be running: for each end a bit of its inbox and a slot it is to hold the borrower's inbox in,
 * and its ends written into its page, closed.
 */
uint32_t bulk_new(struct self *s, struct bulk *b, struct child *lender, struct bulk_end *ends, uint32_t count,
                  const struct bulk_shape *shape);
/*
 * Connects a borrower to connection i, both of which may be running, once the connection is idle:
 * its two blocks zeroed, every buffer to fill lent the borrower,
 * the blocks carved and installed in the borrower, the buffers lent to read read only, and the carves deleted,
 * so that a connection keeps none of the parent's slots;
 * a bit of the borrower's inbox for it, and each side's inbox in the other's table, carved to its bit;
 * then both ends written and both told.
 * KERR_STATE when the lender has not let go of the connection's last borrower yet.
 */
uint32_t bulk_connect(struct self *s, struct bulk *b, uint32_t i, struct child *borrower, struct bulk_end *be);
/*
 * Closes connection i, before or after its borrower is taken down:
 * its blocks uninstalled from the borrower, which a borrower taken down took with it already,
 * the borrower's inbox taken from the lender's table, and the lender told,
 * which says when it has let go, bulk_seen; until then the connection is not idle.
 * A borrower that still runs faults at its next touch of the connection.
 */
uint32_t bulk_close(struct self *s, struct bulk *b, uint32_t i);
/* Whether connection i is closed and its lender has let go of it, so that a borrower may be connected to it. */
int bulk_idle(const struct bulk *b, uint32_t i);
/* The frame back, from the lender and every borrower; the slots and bits given to the lender stay its. */
uint32_t bulk_free(struct self *s, struct bulk *b);

#endif
