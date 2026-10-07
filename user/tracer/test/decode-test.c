/*
 * trace.c on the host, under the sanitizers.
 * The encodings are llvm-objdump's own for clang -march=rv32imac: a sign and a zero form of each width,
 * the four compressed forms a compiler emits, and three c.lw offsets, each hitting other offset bits.
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

static uint8_t dev[256];

static uint32_t dev_read(uint32_t address, unsigned width)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < width; i++) {
        v |= (uint32_t)dev[(address + i) & 0xff] << (8 * i);
    }
    return v;
}

static void dev_write(uint32_t address, uint32_t value, unsigned width)
{
    for (unsigned i = 0; i < width; i++) {
        dev[(address + i) & 0xff] = (uint8_t)(value >> (8 * i));
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
    uint32_t regs[TRACE_REGS] = { 0 };
    struct trace_access a;

    regs[11] = 0x100; /* a1 */
    regs[14] = 0x200; /* a4 */
    regs[8] = 0x40;   /* s0 */
    regs[2] = 0x300;  /* sp */

    for (unsigned i = 0; i < sizeof(decoded) / sizeof(decoded[0]); i++) {
        int len = trace_decode(decoded[i].insn, &a);
        check(len == decoded[i].len && a.store == decoded[i].store && a.width == decoded[i].width &&
                  a.reg == decoded[i].reg && a.sign == decoded[i].sign &&
                  trace_address(&a, regs) == decoded[i].address, decoded[i].name);
    }
    /* The caller knows from the fault's cause that it is a load or store, so anything else is refused. */
    check(trace_decode(0x00000013, &a) == TRACE_BAD, "an instruction of another kind");
    check(trace_decode(0x0000002f, &a) == TRACE_BAD, "an atomic");
    check(trace_decode(0x00000507, &a) == TRACE_BAD, "a floating-point load");
    check(trace_decode(0x00003003, &a) == TRACE_BAD, "a load whose funct3 this does not know");
    check(trace_decode(0x0080, &a) == TRACE_BAD, "a compressed instruction that is not c.lw or c.sw");
}

static const uint8_t pattern[4] = { 0x80, 0xff, 0x34, 0x12 };

static void apply(void)
{
    uint32_t regs[TRACE_REGS] = { 0 };
    struct trace_access a;

    regs[11] = 0x10;
    for (unsigned i = 0; i < sizeof(pattern) / sizeof(pattern[0]); i++) {
        dev[0x10 + i] = pattern[i];
    }
    static const struct {
        uint32_t insn;
        uint32_t out;
        const char *name;
    } loads[] = {
        { 0x00058503, 0xffffff80u, "lb sign-extends" },
        { 0x0005c503, 0x80, "lbu zero-extends" },
        { 0x00059503, 0xffffff80u, "lh sign-extends" },
        { 0x0005d503, 0xff80, "lhu zero-extends" },
        { 0x0005a503, 0x1234ff80u, "lw reads the word" },
    };
    for (unsigned i = 0; i < sizeof(loads) / sizeof(loads[0]); i++) {
        regs[10] = 0;
        trace_decode(loads[i].insn, &a);
        trace_apply(&a, regs, dev_read, dev_write);
        check(regs[10] == loads[i].out, loads[i].name);
    }
    regs[0] = 0; /* lw x0,0(a1) leaves x0 zero */
    trace_decode(0x0005a003, &a);
    trace_apply(&a, regs, dev_read, dev_write);
    check(regs[0] == 0, "x0 stays zero");

    static const struct {
        uint32_t insn;
        uint32_t value;
        unsigned width;
        const char *name;
    } stores[] = {
        { 0x00a58023, 0x12345678, 1, "sb writes one byte" },
        { 0x00a59023, 0x1234abcd, 2, "sh writes two bytes" },
        { 0x00a5a023, 0xdeadbeef, 4, "sw writes the word little-endian" },
    };
    for (unsigned i = 0; i < sizeof(stores) / sizeof(stores[0]); i++) {
        memset(dev, 0, sizeof(dev));
        regs[10] = stores[i].value;
        trace_decode(stores[i].insn, &a);
        trace_apply(&a, regs, dev_read, dev_write);
        uint32_t mask = stores[i].width == 4 ? 0xffffffffu : (1u << (8 * stores[i].width)) - 1;
        check(dev_read(0x10, stores[i].width) == (stores[i].value & mask) &&
                  (stores[i].width == 4 || dev[0x10 + stores[i].width] == 0), stores[i].name);
    }
}

int main(void)
{
    decode();
    apply();
    if (failures != 0) {
        printf("decode-test: %d failures\n", failures);
        return 1;
    }
    printf("decode-test: ok\n");
    return 0;
}
