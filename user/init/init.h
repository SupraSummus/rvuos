/*
 * What the files of the demo share; root.c's main runs their steps in turn.
 * Every file but child.c runs in the root task's process, or its successor's, on the root task's data,
 * and child.c in a process of its own without it.
 */

#ifndef INIT_H
#define INIT_H

#include <stdint.h>

#include "rvuos.h"

/* Slots in the root task's own table, above the ones the kernel filled. */
enum {
    SLOT_LOG_NTFN = BOOT_CAP_COUNT, /* the logger waits here; the log's Irq and the UART's signal it */
    SLOT_LOG_IRQ,       /* the log's line, carved out of the boot grant and bound in place to SLOT_LOG_NTFN */
    SLOT_UART_IRQ,      /* the UART's line, the same way */
    SLOT_LOGGER,        /* the logger thread */
    SLOT_CHILD_DATA,
    SLOT_SHARED,
    SLOT_POOL_MEMORY,   /* the Untyped the child's pool is made of, below which it is destroyed */
    SLOT_POOL,
    SLOT_CHILD_TABLE,
    SLOT_CHILD_PROCESS,
    SLOT_CHILD_THREAD,
    SLOT_UP,   /* the child signals, the root waits */
    SLOT_DOWN, /* the root signals, the child waits */
    SLOT_DOOMED_MEMORY, /* the Untyped of the pool that gets destroyed, and of the one built after it */
    SLOT_DOOMED_POOL,
    SLOT_DOOMED_NTFN,   /* a notification allocated from it */
    SLOT_STALE,         /* a second capability to that same notification */
    SLOT_NEW_POOL,      /* the pool built over the same memory afterwards */
    SLOT_NEW_NTFN,
    SLOT_LEASE,         /* a frame leased to the child by derivation, and taken back by revoking */
    SLOT_LEASE_COPY,    /* a copy beside it, which the revoke leaves alone */
    SLOT_LENT,          /* an Untyped lent to the child, which pools it */
    SLOT_LENT_POOL,     /* what the root task makes of it once it is back */
    SLOT_TIMER_NTFN,    /* what the timer signals, and the spare line's Irq as well */
    SLOT_TIMER_LINE,    /* a timer line, carved out of the boot grant */
    SLOT_TIMER,         /* the timer line bound to SLOT_TIMER_NTFN */
    SLOT_SPARE_LINE,    /* a line nothing drives, carved out of the boot grant */
    SLOT_SPARE_LINE_COPY, /* a second capability to it, inert while the line is bound */
    SLOT_SPARE_IRQ,     /* the line bound to SLOT_TIMER_NTFN */
    SLOT_BOOT_NTFN,     /* a notification in the boot pool, for binding the line again */
    SLOT_SPARE_IRQ_AGAIN,
    SLOT_COUNTER,       /* the clock's frame, read only */
    SLOT_HALF_TIME,     /* the second half of the processor's units, the child's and then the spinners' */
    SLOT_CAPPED_TIME,   /* the same units without spare time, derived from those */
    SLOT_FREEZE_TIME,   /* every unit without spare time, to stop a thread for good */
    SLOT_EXIT_NTFN,     /* what the root task waits on once it has handed everything over */
    SLOT_SUCC_POOL,     /* the successor's pool, table, process and thread */
    SLOT_SUCC_TABLE,
    SLOT_SUCC_PROCESS,
    SLOT_SUCC_THREAD,
    SLOT_REST,          /* what is left of the free RAM, which the next block is taken from */
    SLOT_SPLIT,         /* its upper half, on its way to SLOT_REST */
    SLOT_BLOCK,         /* its lower half, on its way to becoming a frame or a pool */
    SLOT_SPINNER,       /* the spinners' threads, SPINNERS of them */
    /* The dodger's slots are ones the revocation emptied, and the notification it left unused. */
    SLOT_DODGER = SLOT_STALE,             /* the dodger's thread */
    SLOT_DODGER_TIMER = SLOT_DOOMED_NTFN, /* its timer line, bound in place to SLOT_DODGER_NTFN */
    SLOT_DODGER_NTFN = SLOT_NEW_NTFN,     /* what its timer signals */
    /* The prober's are ones the destroy of the boot pool emptied in the successor's table. */
    SLOT_PROBER = SLOT_LOGGER,            /* the successor's thread that faults */
    SLOT_PROBE_NTFN = SLOT_LOG_NTFN,      /* what its faults signal, its watch */
    /* The second core's are ones the destroy of the child's pool emptied. */
    SLOT_CORE_TIME = SLOT_CHILD_TABLE,    /* the second core's units, carved out of the boot grant */
    SLOT_PING = SLOT_UP,                  /* what the ponger waits on */
    SLOT_PONGER = SLOT_CHILD_THREAD,      /* the thread that answers each ping from the second core */
    SLOT_READER = SLOT_CHILD_PROCESS,     /* the thread that reads the shared region there */
    SLOT_LOST = SLOT_DOWN,                /* the thread destroyed while it runs there */
};

/*
 * Slots in the child's table.
 * Which capability sits where is a convention between the two of them
 * and means nothing to the kernel.
 */
enum {
    CHILD_DEBUG = 1,
    CHILD_SHARED,
    CHILD_UP,
    CHILD_DOWN,
    CHILD_DOOMED, /* a notification whose pool the root task destroys */
    CHILD_LEASE,  /* memory derived from the root task's, which revokes it */
    CHILD_LENT,   /* untyped memory the root task lends, derived from its own */
    CHILD_OWN_NTFN, /* allocated from the pool the child makes of it */
    CHILD_LENT_POOL, /* the pool the child makes of the lent memory */
    CHILD_TABLE_SLOTS = 10,
};

/*
 * Region slots. The root task boots with code in 0 and data in 1.
 * Every region costs one PMP entry: the root task maps six and the child four.
 */
#define ROOT_SHARED_SLOT 2
#define ROOT_UART_SLOT 3
#define ROOT_LOG_SLOT 4
#define ROOT_COUNTER_SLOT 5
#define CHILD_CODE_SLOT 0
#define CHILD_DATA_SLOT 1
#define CHILD_SHARED_SLOT 2
#define CHILD_LEASE_SLOT 3

/* The protocol between the two processes. */
#define BIT_REQUEST 0x1u
#define BIT_REPLY   0x2u
#define BIT_DONE    0x4u
#define BIT_CHECK   0x8u /* look at the capability whose pool is gone */
#define BIT_CHECKED 0x10u
#define BIT_POOL    0x20u /* turn the lent memory into a pool */
#define BIT_POOLED  0x40u
#define BIT_LEASE   0x200u /* look at the leased memory, which is gone */
#define BIT_LEASED  0x400u
#define BIT_TIMER   0x80u /* the timer's bit on its own notification */
#define BIT_SPARE   0x100u /* the spare line's bit on that same notification */
#define BIT_FAULT   0x800u /* the prober's faults, on the notification that watches it */
#define BIT_PONG    0x1000u /* the ponger's answer, on the timer's notification */
#define BIT_PING    0x1u    /* what the ponger waits for, on its own notification */
#define MAGIC 0x5eaf00du

/* Ticks are a millisecond on every board so far; long enough to need a few of them. */
#define SLEEP_US 10000u

/* Whose turn it is in the handshake that uses no notification. */
#define TURN_CHILD 0x1u
#define TURN_ROOT  0x2u

/* The shared words: the child's message, the root task's answer, and the turn. */
#define SHARED_MESSAGE 0
#define SHARED_ANSWER  1
#define SHARED_TURN    2

/*
 * How the root task splits the processor's units: it keeps the first ROOT_UNITS,
 * the logger earns the next LOGGER_UNITS, and the child the half after them.
 */
#define ROOT_UNITS   28u
#define LOGGER_UNITS 4u
#define HALF_UNITS   (TIME_UNITS / 2)
_Static_assert(ROOT_UNITS + LOGGER_UNITS == HALF_UNITS, "the child's half follows the logger's units");

/* Threads the root task adds on the child's half once the child is done, each counting as it spins. */
#define SPINNERS 3u
_Static_assert(SLOT_SPINNER + SPINNERS <= ROOT_TABLE_SLOTS, "the root task's slots fit its table");
#define SPINNER_STACK 0x100u
#define SPIN_US 20000u
#define CAPPED_US 80000u
extern volatile uint32_t spins[SPINNERS];

static inline void puts(const char *s)
{
    rv_puts(BOOT_CAP_DEBUG, s);
}

/* Say what was done and whether it went through, and halt the machine if it did not. */
void expect(const char *what, uint32_t status);

/* The slot of what is left of the free RAM; see root.c. */
extern uint32_t rest;

/* The timer's bit, and it alone, on its notification. */
uint32_t timer_wait(void);

/* Sleep on the timer line; see time.c. */
uint32_t sleep_us(uint32_t us);

/* logger.c: start the logger on a stack at sp, which carries the kernel's log to the UART from then on. */
void logger_start(uint32_t uart_base, uint32_t log_base, uint32_t sp);

/* child.c */
void child_main(void);

/* time.c: bind the timer and time it on the clock; the counter's rate and where it is mapped come back. */
void time_demo(uint32_t *hz, uint32_t *counter);

/* units.c: run threads on units of the processor and on spare time, their stacks below the first quarter of the data. */
void units_demo(uint32_t data_base, uint32_t data_size, uint32_t counter, uint32_t hz);

/* cores.c: count the cores, and on a machine of several run threads on the second. */
void cores_demo(uint32_t data_base, uint32_t data_size, uint32_t shared_base, uint32_t counter, uint32_t hz);

/* successor.c */
__attribute__((noreturn)) void hand_over(uint32_t sp);

#endif
