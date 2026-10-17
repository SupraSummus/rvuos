#ifndef RVUOS_FUEL_H
#define RVUOS_FUEL_H

/*
 * The fuel prototype's test, see root.c: a kernel built with make FUEL=1 stops a child whose process's fuel runs dry,
 * and charges a server's work on a client's lent time to the client.
 */

#include "lib/child.h"

/* Every child's page. */
struct fuel_page {
    struct child_page c;
    volatile uint32_t spins;  /* how many rounds of work it did */
    uint32_t lend_to;         /* a client's: the server's thread, RIGHT_X, lent its time while it waits; 0 for none */
    volatile uint32_t lent;   /* a client's: what its lending wait returned, once it returned */
};

/* Work for ever, counting rounds in the page. */
void spin_main(struct child_page *page);
/* Lend the server its time while it waits on its inbox, which nobody signals, then spin. */
void client_main(struct child_page *page);

#endif
