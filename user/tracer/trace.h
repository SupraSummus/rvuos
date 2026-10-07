#ifndef RVUOS_USER_TRACER_TRACE_H
#define RVUOS_USER_TRACER_TRACE_H

/*
 * The load or store a stopped thread faulted at, and how a tracer reads it; see user/phytrace.h.
 * A process that maps no device faults at every register access its code makes:
 * the thread stops where it faulted, its watch signals the tracer, and the tracer carries the access out.
 * This decodes RISC-V's RV32IC loads and stores, plain and compressed,
 * and names the registers an access uses rather than their values,
 * so a tracer reads only the one or two registers a fault needs, not all thirty-two.
 */

#include <stdint.h>

/* The access one instruction makes. */
struct trace_access {
    uint8_t store;  /* nonzero for a store, zero for a load */
    uint8_t width;  /* the bytes it moves: 1, 2 or 4 */
    uint8_t reg;    /* the register it moves data through: rd of a load, rs2 of a store */
    uint8_t sign;   /* a load that sign-extends its result: lb or lh */
    uint8_t base;   /* the register the address is relative to: rs1, or x2 for c.lwsp and c.swsp */
    int32_t offset; /* added to the base, whose register's value is the caller's to read */
};

/*
 * Decode the instruction at insn into *out.
 * Returns 2 or 4, the instruction's length in bytes, for a load or store,
 * and TRACE_BAD for anything else.
 * The caller knows from the fault's cause that the instruction is a load or store,
 * so one that is not, or one whose encoding this does not know, is refused rather than guessed.
 */
#define TRACE_BAD (-1)
int trace_decode(uint32_t insn, struct trace_access *out);

/*
 * --- The record a traced process and the root task that serves it share. ---
 *
 * A traced process maps no device, so every register access its code makes faults to its watcher;
 * the watcher decodes the access into a request below, the root task carries it out on its own mapping,
 * and leaves the answer in the request. The two sides are the driver's osi.c's watcher and root.c;
 * a tracer for the harness of user/phyblob/ would carry the access out itself, with trace_decode above.
 */

/* What a request asks: a read whose answer comes back, or a write. */
#define TRACE_READ  0u
#define TRACE_WRITE 1u

/* The RISC-V causes a device access faults with, a load's and a store's; a watcher serves only these. */
#define TRACE_CAUSE_LOAD  5u
#define TRACE_CAUSE_STORE 7u

#define TRACE_FLAG_BATCH   0x1u /* came in one wake with another thread's fault, so its order among them is not known */
#define TRACE_FLAG_CHANGED 0x2u /* in a cycle's turn: this access's value changed from one turn to the next */

/* The root task's verdict on a request, in trace_req.status. */
#define TRACE_DONE    0u /* the access was carried out, and a read's answer is in value */
#define TRACE_REFUSED 1u /* it was no access the root may carry out: no device, a width or an address it refuses */

struct trace_req {
    uint32_t op;      /* TRACE_READ or TRACE_WRITE */
    uint32_t width;   /* the bytes it moves: 1, 2 or 4 */
    uint32_t address;
    uint32_t value;   /* a write's value; a read's answer, the width bytes zero-extended, is left here */
    uint32_t flags;   /* TRACE_FLAG_ bits */
    uint32_t status;  /* the root task's: TRACE_DONE or TRACE_REFUSED */
    uint32_t pc;      /* where in the traced code the instruction lies, for the log */
    uint32_t thread;  /* which of the traced process's threads made the access, for the log */
};

/*
 * The request and the two counters the sides share, as child_page's asked and answered:
 * the watcher bumps asked once the request is whole, with a release fence before it,
 * and the root sets answered to asked once the answer is in the request, likewise;
 * so the root tells a trace request from a state report on the same bit of its inbox.
 */
struct trace_share {
    volatile uint32_t asked;
    volatile uint32_t answered;
    struct trace_req req;
    /*
     * The watcher's, for a fault it cannot even put into a request: a real fault, not a device access,
     * or an instruction the decoder refuses. The root task prints it before it halts.
     */
    volatile uint32_t cannot;
    struct trace_giveup {
        uint32_t cause;   /* the fault's cause, as the kernel reports it */
        uint32_t pc;
        uint32_t address;
        uint32_t thread;
    } giveup;
};

/*
 * --- The stream of accesses, compressed to the lines a console can carry. ---
 *
 * A busy-wait turns over a few addresses, so the same accesses come again and again with values that change;
 * printing each would drown the console and hide the one-time writes the trace is for.
 * The stream keeps the last two turns of a short cycle at most: when it sees the same turn twice,
 * it counts the turns instead of printing them, and prints one turn with the count when the cycle ends.
 * A record is printed only once a cycle longer than the history is ruled out, so nothing is printed twice.
 */
#define TRACE_MAXP 6 /* the longest cycle looked for, a guess at the modem's loops; a longer one prints as records */

/* One access, as the stream keeps it; value is kept too, since a cycle's turn may carry different values. */
struct trace_rec {
    uint32_t op, address, width, value, pc, thread, flags;
};

/* Where a compressed stream's pieces go: a record, and a cycle's one turn with how many times it turned. */
struct trace_sink {
    void (*record)(void *ctx, const struct trace_rec *e);
    void (*cycle)(void *ctx, uint32_t turns, uint32_t len, const struct trace_rec *turn);
    void *ctx;
};

struct trace_cycle {
    struct trace_rec head[2 * TRACE_MAXP]; /* records not yet printed, before a cycle is found */
    uint32_t hn;
    struct trace_rec turn[TRACE_MAXP]; /* the active cycle's one turn */
    uint32_t len;                      /* its length, 0 when none */
    uint32_t records;                  /* records it has taken, so records / len turns */
};

void trace_cycle_init(struct trace_cycle *c);
void trace_cycle_feed(struct trace_cycle *c, const struct trace_rec *e, const struct trace_sink *sink);
void trace_cycle_end(struct trace_cycle *c, const struct trace_sink *sink);

#endif
