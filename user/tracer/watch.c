/* The device access a faulted thread made, decoded and carried out on its behalf; see watch.h. */

#include <stdint.h>

#include "rvuos.h"
#include "tracer/watch.h"

static int32_t sign_extend(uint32_t value, unsigned bits)
{
    uint32_t m = 1u << (bits - 1);
    return (int32_t)((value ^ m) - m);
}

int trace_fault(uint32_t thread_cap, struct trace_req *req, struct trace_pending *b)
{
    uint32_t cause, pc, addr, status, base;
    if (rv_thread_fault(thread_cap, &cause, &pc, &addr, &status) != KERR_OK) {
        return -1;
    }
    if (cause != TRACE_CAUSE_LOAD && cause != TRACE_CAUSE_STORE) {
        return -1;
    }
    /* The instruction that faulted: two bytes first, and two more only if the low bits say a 32-bit one. */
    const uint8_t *code = (const uint8_t *)(uintptr_t)pc;
    uint32_t insn = (uint32_t)code[0] | ((uint32_t)code[1] << 8);
    if ((insn & 0x3u) == 0x3u) {
        insn |= ((uint32_t)code[2] << 16) | ((uint32_t)code[3] << 24);
    }
    struct trace_access a;
    int len = trace_decode(insn, &a);
    if (len == TRACE_BAD) {
        return -1;
    }
    if (rv_thread_read_reg(thread_cap, a.base, &base) != KERR_OK) {
        return -1;
    }
    req->op = a.store ? TRACE_WRITE : TRACE_READ;
    req->width = a.width;
    req->address = base + (uint32_t)a.offset;
    /* The address the kernel reports is the instruction's own; a decoder that computes another is not to be trusted. */
    if (addr != 0 && addr != req->address) {
        return -1;
    }
    req->value = 0;
    req->flags = 0;
    req->status = TRACE_DONE;
    req->pc = pc;
    req->thread = 0; /* the caller names the thread, which only it knows */
    if (a.store && rv_thread_read_reg(thread_cap, a.reg, &req->value) != KERR_OK) {
        return -1;
    }
    b->reg = a.reg;
    b->pc = pc;
    b->len = (uint32_t)len;
    b->sign = a.sign;
    return 0;
}

int trace_done(uint32_t thread_cap, const struct trace_req *req, const struct trace_pending *b)
{
    if (req->op == TRACE_READ && b->reg != 0) {
        uint32_t value = req->value;
        if (b->sign) {
            value = (uint32_t)sign_extend(value, 8u * req->width);
        }
        if (rv_thread_write_reg(thread_cap, b->reg, value) != KERR_OK) {
            return -1;
        }
    }
    /* The pc past the instruction, so the resume goes on after it, then the thread runs again. */
    if (rv_thread_write_reg(thread_cap, THREAD_REG_PC, b->pc + b->len) != KERR_OK) {
        return -1;
    }
    return rv_thread_resume(thread_cap) == KERR_OK ? 0 : -1;
}
