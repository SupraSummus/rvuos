#ifndef RVUOS_BENCH_H
#define RVUOS_BENCH_H

/* The benchmark, see root.c: the pages of its children. */

#include "lib/chan.h"
#include "lib/child.h"

/* The channel's packets: as long as a small datagram, in slots a little longer. */
#define PACKET    64u
#define LINK_SIZE 0x1000u
#define LINK_SLOT 128u

enum {
    PING_DONE = CHILD_RUNNING + 1, /* sent its rounds, and the page says how long they took */
};

/* The ping and the echo of the channel measure; the echo uses the link alone. */
struct ping_page {
    struct child_page c;
    struct chan_end link;
    uint32_t clock; /* the ping's slot of the clock */
    uint32_t rounds;
    /* From the ping: the clock's counts over the rounds, and its rate. */
    volatile uint32_t counts, hz;
};

void pong_main(struct child_page *page);
void spin_main(struct child_page *page);
void ping_main(struct child_page *page);
void echo_main(struct child_page *page);

#endif
