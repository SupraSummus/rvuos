#ifndef RVUOS_LIB_CHILD_H
#define RVUOS_LIB_CHILD_H

/*
 * Processes a parent builds, from both sides.
 *
 * A child runs its parent's code but keeps no global: its state lies on its stack or in its page.
 * Its memory is one frame, its data, with its page at the base and its stack at the top,
 * and it starts at its entry with a0 at its page.
 * The data is a block of its own, or carved from the parent's room for its children's data, see self_room.
 * The parent has the data installed too, in a region for each child or in its room for all,
 * so the page is what the two share:
 * the parent writes into it what it gave the child, and the child its state.
 * The child writes its text into the kernel's log, through CHILD_LOG, which may write and not halt the machine.
 * Whatever signals another's inbox holds it carved to the one bit that is its own,
 * so the bits a wait returns say who signalled, and a signaller names every bit, not knowing which.
 * Past the first slots, regions and bits below, the parent hands the child's out and says in the page which,
 * so the page's type is all the two agree on.
 */

#include <stdint.h>

#include "lib/log.h"
#include "lib/self.h"

/* The first slots of every child's table. */
enum {
    CHILD_NULL,
    CHILD_INBOX,  /* the notification it waits on, RIGHT_R */
    CHILD_PARENT, /* its parent's notification, RIGHT_W, carved to the child's bit there */
    CHILD_TIMER,  /* an Irq on a timer line of its own, which signals CHILD_INBOX */
    CHILD_SELF,   /* its own process, RIGHT_W, for regions of its own */
    CHILD_LOG,    /* the kernel's log, Debug with RIGHT_W alone: it writes, and cannot halt the machine */
    CHILD_FIRST,  /* the slots the parent hands out begin here */
};

/* The first regions of every child's process. */
enum {
    CHILD_REGION_CODE,
    CHILD_REGION_DATA,
};

/* The first bits of every child's inbox. */
#define CHILD_BIT_TIMER  0x1u /* the child arms its timer with it */
#define CHILD_BIT_PARENT 0x2u /* the parent wrote something into the page */

/* A child's state; each child adds states of its own from CHILD_RUNNING up. */
enum {
    CHILD_STARTING,
    CHILD_FAILED,
    CHILD_RUNNING,
};

/* What every child's page starts with; a program's page type begins with it. */
struct child_page {
    volatile uint32_t state;
    volatile uint32_t step;     /* where it got to, for a failure */
    volatile uint32_t detail;   /* a value that step read, for a failure */
    volatile uint32_t asked;    /* the parent's: how many times it asked whether the child runs, see child_check */
    volatile uint32_t answered; /* the child's: the last of those it answered, see child_answer */
};

/* --- The parent's side. --- */

#define CHILD_POOL_SIZE   0x1000u /* the pool of a child's objects: a table of up to CHILD_TABLE_MAX slots and the rest */
#define CHILD_TABLE_MAX   64u

/*
 * A child, as its parent keeps it, in six slots: its four objects, and the Untypeds of its pool and data.
 * What only building it needs, its pool, data frame, timer and units, is deleted once used;
 * what was copied, installed or bound from them stays, below the Untypeds and the boot's capabilities.
 */
struct child {
    const char *name;
    uint32_t table, process, thread, inbox; /* the parent's slots of its objects */
    struct block data, pool;
    struct child_page *page;                /* at the base of its data, where the parent sees it too */
    uint32_t region;                        /* the parent's region its data is installed in, its room's if in_room */
    int in_room;                            /* its data is carved from the parent's room, not a block of its own */
    uint32_t bit_page, bit_fault;           /* its bits on the parent's inbox: a new state, and a fault */
    uint32_t unit, units;                   /* the units of time it earns */
    /* What of the child's is handed out, a bit each or a count. */
    uint32_t table_slots, slots_given, regions, bits;
    /* The last state the parent saw. */
    uint32_t told;
};

/*
 * A child of table_slots slots and data_size bytes of data, not started:
 * its objects from a pool of its own, its first slots and regions filled,
 * and its data zeroed and installed in a region of the parent's too, or carved from the parent's room,
 * for the parent to fill the page.
 * A failure takes back what was made.
 */
uint32_t child_new(struct self *s, struct child *c, const char *name, uint32_t table_slots, uint32_t data_size);
/* A copy of the parent's slot, with rights at most, in the child's next free slot, which *at says. */
uint32_t child_give(struct self *s, struct child *c, uint32_t slot, uint32_t rights, uint32_t *at);
/* A notification of the parent's, to signal with RIGHT_W alone and only bits, in the child's next free slot. */
uint32_t child_give_bits(struct self *s, struct child *c, uint32_t ntfn, uint32_t bits, uint32_t *at);
/* The child's next free slot, left empty, for the parent to fill later, as child_put_bits does. */
uint32_t child_slot(struct self *s, struct child *c, uint32_t *at);
/* As child_give_bits, into the child's slot at, which holds nothing. */
uint32_t child_put_bits(struct self *s, struct child *c, uint32_t at, uint32_t ntfn, uint32_t bits);
/* A frame installed in the child's next free region, which *region says. */
uint32_t child_map(struct self *s, struct child *c, uint32_t frame, uint32_t rights, uint32_t *region);
/*
 * A region the parent handed out uninstalled and free again, the child reaching nothing through it from then on:
 * child_unmap from one child and child_map into another hands memory over.
 */
uint32_t child_unmap(struct self *s, struct child *c, uint32_t region);
/* A region of the child's for the child to install frames into itself, through CHILD_SELF. */
uint32_t child_region(struct self *s, struct child *c, uint32_t *region);
/* A bit of the child's inbox, for something to signal it with. */
uint32_t child_bit(struct self *s, struct child *c, uint32_t *bit);
/*
 * Starts the child at entry, with a0 at its page, its stack at the top of its data,
 * earning units of the first core, and its faults signalling the parent's inbox with bit_fault.
 */
uint32_t child_start(struct self *s, struct child *c, void (*entry)(struct child_page *), uint32_t units);
/* Tells a running child the parent wrote into its page. */
uint32_t child_tell(const struct child *c);
/* 1 if the child's state changed since the last call, *state the new one. */
int child_poll(struct child *c, uint32_t *state);
/*
 * Whether the child answered the last check, or was never asked; then asks again, and tells it.
 * Checked every so often, a child that spins or waits for what never comes misses one.
 */
int child_check(struct child *c);
/*
 * Takes the child down: its pool, and with it its objects, timer line and units, and every capability to them;
 * its data, wherever it was installed; and the parent's slots, region and bits it used.
 * What the parent gave it stays the parent's.
 */
uint32_t child_free(struct self *s, struct child *c);

/* --- The child's side. --- */

/* The child's text, into the kernel's log. */
static inline struct out child_out(void)
{
    return log_out(CHILD_LOG);
}
/* A new state into the page, and the parent told. */
void child_report(struct child_page *p, uint32_t state);
/* Answers the parent's last check; called each time round the child's loop. */
static inline void child_answer(struct child_page *p)
{
    p->answered = p->asked;
}
/* Reports state and waits for good; the parent decides what comes next. */
__attribute__((noreturn)) void child_stop(struct child_page *p, uint32_t state);
/* Reports CHILD_FAILED, with where and what. */
__attribute__((noreturn)) void child_fail(struct child_page *p, uint32_t step, uint32_t detail);
/* Waits us microseconds on the child's timer, and returns the other bits that came meanwhile, for the caller to keep. */
uint32_t child_sleep(uint32_t us);

#endif
