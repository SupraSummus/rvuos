/*
 * The children of the fuel prototype's test, see root.c.
 */

#include "fuel.h"

void spin_main(struct child_page *c)
{
    struct fuel_page *p = (struct fuel_page *)c;
    for (;;) {
        for (volatile uint32_t i = 0; i < 1000u; i++) {
        }
        p->spins++;
    }
}

void client_main(struct child_page *c)
{
    struct fuel_page *p = (struct fuel_page *)c;
    uint32_t bits = 0;
    p->lent = rv_lend(CHILD_INBOX, p->lend_to, &bits) | 0x100u;
    spin_main(c);
}
