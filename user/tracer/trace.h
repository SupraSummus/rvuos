#ifndef RVUOS_USER_TRACER_TRACE_H
#define RVUOS_USER_TRACER_TRACE_H

/*
 * The load or store a stopped thread faulted at, and how a tracer carries it out; see user/phytrace.h.
 * A process that maps no device faults at every register access its code makes:
 * the thread stops where it faulted, its watch signals the tracer, and the tracer emulates the access.
 * This decodes RISC-V's RV32IC loads and stores, plain and compressed, and applies one to a thread's registers.
 * It names the registers an access uses rather than their values,
 * so a tracer reads only the one or two registers a fault needs, not all thirty-two.
 */

#include <stdint.h>

/* The general registers a frame holds, as OP_THREAD_READ_REG numbers them: x0 to x31. */
#define TRACE_REGS 32

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

/* The address *a reaches, from the value of its base register. */
uint32_t trace_address(const struct trace_access *a, const uint32_t *regs);

/* The device side, the tracer's own mapping of the frame the watched process lacks. */
typedef uint32_t (*trace_read)(uint32_t address, unsigned width);
typedef void (*trace_write)(uint32_t address, uint32_t value, unsigned width);

/*
 * Carry out *a through read and write, and leave a load's value, sign-extended where the instruction says,
 * in regs[rd]. x0 stays zero. A store takes regs[rs2] and writes width bytes of it.
 */
void trace_apply(const struct trace_access *a, uint32_t *regs, trace_read read, trace_write write);

#endif
