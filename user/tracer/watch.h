#ifndef RVUOS_USER_TRACER_WATCH_H
#define RVUOS_USER_TRACER_WATCH_H

/*
 * A served fault: what the watcher of user/wifi/esp32c6/osi.c decodes from a stopped thread,
 * and what it leaves behind once the root task has carried the access out; see user/tracer/trace.h.
 * The watcher takes no lock and puts nothing on the heap in this path,
 * since a thread it serves may hold one of the adapter's, and a fault keeps the lock with the thread.
 */

#include <stdint.h>

#include "trace.h"

/* What a served fault keeps between the decode and the resume, besides the request. */
struct trace_pending {
    uint32_t reg;  /* the register a read leaves its answer in, or a write took its value from */
    uint32_t pc;   /* the instruction's address */
    uint32_t len;  /* its length, 2 or 4 bytes */
    uint32_t sign; /* a read that sign-extends its answer */
};

/*
 * Decode the device access a stopped thread faulted at into *req, and what *b keeps for the resume.
 * Returns 0 for a device access, or nonzero for anything else: no access fault, or an instruction this cannot read.
 * The thread's registers are read through its capability, so the address and a write's value are the access's.
 */
int trace_fault(uint32_t thread_cap, struct trace_req *req, struct trace_pending *b);

/* Leave the answer in the thread, its program counter past the instruction, and resume it. 0, or the failing step. */
int trace_done(uint32_t thread_cap, const struct trace_req *req, const struct trace_pending *b);

#endif
