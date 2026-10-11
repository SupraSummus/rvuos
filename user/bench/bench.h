#ifndef RVUOS_BENCH_H
#define RVUOS_BENCH_H

/* The benchmark, see root.c: the pages of its children. */

#include "lib/bulk.h"
#include "lib/chan.h"
#include "lib/child.h"

/*
 * The channel's packets: as long as a small datagram, in slots a little longer;
 * and a datagram's worth, 1 KiB, two slots a ring, so that the ping's next packet finds room
 * however late the echo gives back the slot of the last.
 */
#define PACKET       64u
#define LINK_SIZE    0x1000u
#define LINK_SLOT    128u
#define PACKET_1K    0x400u
#define LINK_SIZE_1K 0x2000u

enum {
    PING_DONE = CHILD_RUNNING + 1, /* sent its rounds, and the page says how long they took */
};

/* The ping and the echo of the channel measures; the echo uses the link alone. */
struct ping_page {
    struct child_page c;
    struct chan_end link;
    uint32_t clock; /* the ping's slot of the clock */
    uint32_t rounds;
    uint32_t len;   /* the bytes of each packet, PACKET_1K at most */
    /* From the ping: the clock's counts over the rounds, and its rate. */
    volatile uint32_t counts, hz;
};

/* The lender and the borrower of the bulk measure: a buffer of PACKET_1K bytes lent to read and handed back. */
struct lend_page {
    struct child_page c;
    struct bulk_end link; /* the lender's one connection, or the borrower's end of it */
    uint32_t clock;       /* the lender's slot of the clock */
    uint32_t rounds;
    volatile uint32_t counts, hz;
};

void pong_main(struct child_page *page);
void spin_main(struct child_page *page);
void ping_main(struct child_page *page);
void echo_main(struct child_page *page);
void lend_main(struct child_page *page);
void borrow_main(struct child_page *page);

#endif
