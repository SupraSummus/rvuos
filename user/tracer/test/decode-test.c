/*
 * trace.c on the host, under the sanitizers: the decoder, and the stream's compression.
 * The encodings are llvm-objdump's own for clang -march=rv32imac: a sign and a zero form of each width,
 * the four compressed forms a compiler emits, and three c.lw offsets, each hitting other offset bits.
 * The compression is driven over short sequences a busy-wait makes, and over a value that changes.
 */

#include <stdio.h>
#include <string.h>

#include "trace.h"

static int failures;

static void check(int ok, const char *what)
{
    if (!ok) {
        printf("decode-test: FAIL %s\n", what);
        failures++;
    }
}

/* One instruction and what trace_decode should make of it, the address from the registers below. */
static const struct {
    uint32_t insn;
    int len, store, width, reg, sign;
    uint32_t address;
    const char *name;
} decoded[] = {
    { 0x0085a503, 4, 0, 4, 10, 0, 0x108, "lw a0,8(a1)" },
    { 0xfec5ae23, 4, 1, 4, 12, 0, 0x0fc, "sw a2,-4(a1)" },
    { 0x00370783, 4, 0, 1, 15, 1, 0x203, "lb a5,3(a4)" },
    { 0x00371783, 4, 0, 2, 15, 1, 0x203, "lh a5,3(a4)" },
    { 0x00374783, 4, 0, 1, 15, 0, 0x203, "lbu a5,3(a4)" },
    { 0x00375783, 4, 0, 2, 15, 0, 0x203, "lhu a5,3(a4)" },
    { 0x01140123, 4, 1, 1, 17, 0, 0x42, "sb a7,2(s0)" },
    { 0x01141123, 4, 1, 2, 17, 0, 0x42, "sh a7,2(s0)" },
    { 0x01142123, 4, 1, 4, 17, 0, 0x42, "sw a7,2(s0)" },
    { 0x41c8, 2, 0, 4, 10, 0, 0x104, "c.lw a0,4(a1)" },
    { 0x41a8, 2, 0, 4, 10, 0, 0x140, "c.lw a0,64(a1)" },
    { 0x5de8, 2, 0, 4, 10, 0, 0x17c, "c.lw a0,124(a1)" },
    { 0xc1c8, 2, 1, 4, 10, 0, 0x104, "c.sw a0,4(a1)" },
    { 0xdde8, 2, 1, 4, 10, 0, 0x17c, "c.sw a0,124(a1)" },
    { 0x4512, 2, 0, 4, 10, 0, 0x304, "c.lwsp a0,4(sp)" },
    { 0x557e, 2, 0, 4, 10, 0, 0x3fc, "c.lwsp a0,252(sp)" },
    { 0xc22a, 2, 1, 4, 10, 0, 0x304, "c.swsp a0,4(sp)" },
    { 0xdfaa, 2, 1, 4, 10, 0, 0x3fc, "c.swsp a0,252(sp)" },
};

static void decode(void)
{
    uint32_t regs[32] = { 0 };
    struct trace_access a;

    regs[11] = 0x100; /* a1 */
    regs[14] = 0x200; /* a4 */
    regs[8] = 0x40;   /* s0 */
    regs[2] = 0x300;  /* sp */

    for (unsigned i = 0; i < sizeof(decoded) / sizeof(decoded[0]); i++) {
        int len = trace_decode(decoded[i].insn, &a);
        check(len == decoded[i].len && a.store == decoded[i].store && a.width == decoded[i].width &&
                  a.reg == decoded[i].reg && a.sign == decoded[i].sign &&
                  regs[a.base] + (uint32_t)a.offset == decoded[i].address, decoded[i].name);
    }
    /* The caller knows from the fault's cause that it is a load or store, so anything else is refused. */
    check(trace_decode(0x00000013, &a) == TRACE_BAD, "an instruction of another kind");
    check(trace_decode(0x0000002f, &a) == TRACE_BAD, "an atomic");
    check(trace_decode(0x00000507, &a) == TRACE_BAD, "a floating-point load");
    check(trace_decode(0x00003003, &a) == TRACE_BAD, "a load whose funct3 this does not know");
    check(trace_decode(0x0080, &a) == TRACE_BAD, "a compressed instruction that is not c.lw or c.sw");
}

/* The stream's compression: a cycle prints one turn, how often it turned, and the records past the full turns. */
static char cyc_log[256];
static unsigned cyc_n;

static char cyc_char(uint32_t pc)
{
    return pc == 0x100 ? 'A' : pc == 0x200 ? 'B' : 'C';
}

static void cyc_note(const struct trace_rec *e)
{
    cyc_n += (unsigned)snprintf(cyc_log + cyc_n, sizeof(cyc_log) - cyc_n, " %c%u%s", cyc_char(e->pc), e->value,
                                e->flags & TRACE_FLAG_CHANGED ? "*" : "");
}

static void cyc_sink_rec(void *ctx, const struct trace_rec *e)
{
    (void)ctx;
    cyc_note(e);
}

static void cyc_sink_cycle(void *ctx, uint32_t turns, uint32_t len, const struct trace_rec *turn)
{
    (void)ctx;
    cyc_n += (unsigned)snprintf(cyc_log + cyc_n, sizeof(cyc_log) - cyc_n, " cycle x%u of %u:", turns, len);
    for (uint32_t i = 0; i < len; i++) {
        cyc_note(&turn[i]);
    }
}

static const struct trace_sink cyc_sink = { cyc_sink_rec, cyc_sink_cycle, 0 };

static void cyc_feed(struct trace_cycle *c, const char *seq, const uint32_t *vals)
{
    struct trace_rec e = { .op = TRACE_READ, .width = 4 };
    for (unsigned i = 0; seq[i] != 0; i++) {
        e.pc = seq[i] == 'A' ? 0x100 : seq[i] == 'B' ? 0x200 : 0x300;
        e.address = e.pc << 4;
        e.value = vals ? vals[i] : 0;
        e.flags = 0;
        trace_cycle_feed(c, &e, &cyc_sink);
    }
}

static void cyc_case(const char *seq, const uint32_t *vals, const char *want)
{
    struct trace_cycle c;
    trace_cycle_init(&c);
    cyc_n = 0;
    cyc_feed(&c, seq, vals);
    trace_cycle_end(&c, &cyc_sink);
    cyc_log[cyc_n] = 0;
    check(strcmp(cyc_log, want) == 0, seq);
    if (strcmp(cyc_log, want) != 0) {
        printf("decode-test: cycle %s gave '%s', wanted '%s'\n", seq, cyc_log, want);
    }
}

static void cycle(void)
{
    cyc_case("ABABABC", 0, " cycle x3 of 2: A0 B0 C0");   /* the turn once, its count, then the breaking record */
    cyc_case("ABABAC", 0, " cycle x2 of 2: A0 B0 A0 C0"); /* the part turn past the full turns is not lost */
    cyc_case("ABC", 0, " A0 B0 C0");                      /* no cycle: every record stands */
    cyc_case("ABABABAB", 0, " cycle x4 of 2: A0 B0");
    static const uint32_t vals[] = { 0, 0, 0, 1, 7 };     /* a poll's value: the turn keeps the last, and changed */
    cyc_case("AAAAC", vals, " cycle x4 of 1: A1* C7");
}

int main(void)
{
    decode();
    cycle();
    if (failures != 0) {
        printf("decode-test: %d failures\n", failures);
        return 1;
    }
    printf("decode-test: ok\n");
    return 0;
}
