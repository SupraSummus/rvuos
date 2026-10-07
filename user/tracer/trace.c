/* The faulting load or store a tracer carries out; see trace.h. */

#include "trace.h"

static int32_t sign_extend(uint32_t value, unsigned bits)
{
    value &= (1u << bits) - 1;
    uint32_t m = 1u << (bits - 1);
    return (int32_t)((value ^ m) - m);
}

int trace_decode(uint32_t insn, struct trace_access *out)
{
    uint32_t op = insn & 0x3;

    if (op != 0x3) {
        /* A compressed instruction, 16 bits wide. */
        unsigned f3 = (insn >> 13) & 0x7;
        unsigned base = 2; /* the stack pointer, which the compressed forms that name it use */
        unsigned reg;
        if (op == 0 && (f3 == 2 || f3 == 6)) {
            /* c.lw and c.sw: rs1' and rd'/rs2' are x8 to x15; the offset's bits 6, 5:3 and 2. */
            base = 8 + ((insn >> 7) & 0x7);
            reg = 8 + ((insn >> 2) & 0x7);
            out->offset = (int32_t)(((insn >> 5) & 1) << 6 | ((insn >> 10) & 0x7) << 3 | ((insn >> 6) & 1) << 2);
        } else if (op == 2 && f3 == 2) {
            /* c.lwsp: rd from bits 11:7, zero reserved; the offset's bits 4:2, 5 and 7:6. */
            reg = (insn >> 7) & 0x1f;
            if (reg == 0) {
                return TRACE_BAD;
            }
            out->offset = (int32_t)(((insn >> 2) & 0x3) << 6 | ((insn >> 12) & 1) << 5 | ((insn >> 4) & 0x7) << 2);
        } else if (op == 2 && f3 == 6) {
            /* c.swsp: rs2 from bits 6:2; the offset's bits 7:6 and 5:2. */
            reg = (insn >> 2) & 0x1f;
            out->offset = (int32_t)(((insn >> 7) & 0x3) << 6 | ((insn >> 9) & 0xf) << 2);
        } else {
            return TRACE_BAD;
        }
        out->store = f3 == 6;
        out->width = 4;
        out->reg = (uint8_t)reg;
        out->sign = 0;
        out->base = (uint8_t)base;
        return 2;
    }

    unsigned opcode = insn & 0x7f;
    unsigned f3 = (insn >> 12) & 0x7;
    if (opcode == 0x03) { /* LOAD */
        if (f3 == 3 || f3 == 6 || f3 == 7) {
            return TRACE_BAD;
        }
        out->store = 0;
        out->width = (f3 & 0x3) == 0 ? 1 : (f3 & 0x3) == 1 ? 2 : 4;
        out->reg = (insn >> 7) & 0x1f;
        out->sign = f3 == 0 || f3 == 1;
        out->base = (insn >> 15) & 0x1f;
        out->offset = sign_extend(insn >> 20, 12);
        return 4;
    }
    if (opcode == 0x23) { /* STORE */
        if (f3 > 2) {
            return TRACE_BAD;
        }
        uint32_t imm = ((insn >> 25) << 5) | ((insn >> 7) & 0x1f);
        out->store = 1;
        out->width = f3 == 0 ? 1 : f3 == 1 ? 2 : 4;
        out->reg = (insn >> 20) & 0x1f;
        out->sign = 0;
        out->base = (insn >> 15) & 0x1f;
        out->offset = sign_extend(imm, 12);
        return 4;
    }
    return TRACE_BAD;
}

uint32_t trace_address(const struct trace_access *a, const uint32_t *regs)
{
    return regs[a->base] + (uint32_t)a->offset;
}

void trace_apply(const struct trace_access *a, uint32_t *regs, trace_read read, trace_write write)
{
    uint32_t address = trace_address(a, regs);
    if (a->store) {
        write(address, regs[a->reg], a->width);
        return;
    }
    uint32_t value = read(address, a->width);
    if (a->sign) {
        value = (uint32_t)sign_extend(value, 8 * a->width);
    }
    if (a->reg != 0) {
        regs[a->reg] = value;
    }
}
