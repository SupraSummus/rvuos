/* The load or store a faulting instruction is, and the stream's short-cycle compression; see trace.h. */

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

/* --- The stream's short-cycle compression; see trace.h. --- */

static int rec_same(const struct trace_rec *a, const struct trace_rec *b)
{
    return a->op == b->op && a->address == b->address && a->width == b->width && a->pc == b->pc;
}

void trace_cycle_init(struct trace_cycle *c)
{
    c->hn = 0;
    c->len = 0;
    c->records = 0;
}

/* The shortest p whose last two turns in the head are equal, or 0. */
static uint32_t cycle_find(const struct trace_rec *head, uint32_t hn)
{
    for (uint32_t p = 1; p <= TRACE_MAXP && 2 * p <= hn; p++) {
        int same = 1;
        for (uint32_t i = 0; i < p; i++) {
            if (!rec_same(&head[hn - 2 * p + i], &head[hn - p + i])) {
                same = 0;
                break;
            }
        }
        if (same) {
            return p;
        }
    }
    return 0;
}

/* Hand the active cycle to the sink, if any, and clear it: its full turns once, then the records past them. */
static void cycle_emit(struct trace_cycle *c, const struct trace_sink *sink)
{
    if (c->len == 0) {
        return;
    }
    sink->cycle(sink->ctx, c->records / c->len, c->len, c->turn);
    uint32_t rem = c->records % c->len;
    for (uint32_t i = 0; i < rem; i++) {
        sink->record(sink->ctx, &c->turn[i]);
    }
    c->len = 0;
    c->records = 0;
}

void trace_cycle_feed(struct trace_cycle *c, const struct trace_rec *e, const struct trace_sink *sink)
{
    if (c->len != 0) {
        struct trace_rec *turn = &c->turn[c->records % c->len];
        if (rec_same(e, turn)) {
            if (e->value != turn->value) {
                turn->flags |= TRACE_FLAG_CHANGED;
                turn->value = e->value;
            }
            c->records++;
            return;
        }
        cycle_emit(c, sink);
    }
    /* The head keeps two turns of the longest cycle at most, so a cycle is found before its start is printed. */
    if (c->hn == 2 * TRACE_MAXP) {
        sink->record(sink->ctx, &c->head[0]);
        for (uint32_t i = 1; i < c->hn; i++) {
            c->head[i - 1] = c->head[i];
        }
        c->hn--;
    }
    c->head[c->hn++] = *e;
    uint32_t p = cycle_find(c->head, c->hn);
    if (p != 0) {
        for (uint32_t i = 0; i + 2 * p < c->hn; i++) {
            sink->record(sink->ctx, &c->head[i]);
        }
        /* The turn keeps the last turn's values, and says whether they changed from the turn before it. */
        for (uint32_t i = 0; i < p; i++) {
            c->turn[i] = c->head[c->hn - p + i];
            if (c->head[c->hn - 2 * p + i].value != c->turn[i].value) {
                c->turn[i].flags |= TRACE_FLAG_CHANGED;
            }
        }
        c->len = p;
        c->records = 2 * p;
        c->hn = 0;
    }
}

void trace_cycle_end(struct trace_cycle *c, const struct trace_sink *sink)
{
    for (uint32_t i = 0; i < c->hn; i++) {
        sink->record(sink->ctx, &c->head[i]);
    }
    c->hn = 0;
    cycle_emit(c, sink);
}
