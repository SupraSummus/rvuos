/*
 * Physical Memory Protection register access.
 *
 * PMP CSRs are addressed by name in the instruction encoding,
 * so indexed access goes through a switch.
 * The tables below are the only place that lists them.
 */

#include "csr.h"
#include "kernel.h"
#include "layout.h"
#include "pmp.h"

_Static_assert(PMP_MAX_ENTRIES >= 1 && PMP_MAX_ENTRIES <= PMP_CSR_ENTRIES,
               "pmp.c addresses at most sixteen entries");

static uint32_t pmpaddr_read(unsigned idx)
{
    switch (idx) {
#define CASE(n) case n: return csr_read(pmpaddr##n);
        CASE(0)  CASE(1)  CASE(2)  CASE(3)
        CASE(4)  CASE(5)  CASE(6)  CASE(7)
        CASE(8)  CASE(9)  CASE(10) CASE(11)
        CASE(12) CASE(13) CASE(14) CASE(15)
#undef CASE
    default:
        kpanic("pmpaddr index out of range");
    }
}

static void pmpaddr_write(unsigned idx, uint32_t v)
{
    switch (idx) {
#define CASE(n) case n: csr_write(pmpaddr##n, v); return;
        CASE(0)  CASE(1)  CASE(2)  CASE(3)
        CASE(4)  CASE(5)  CASE(6)  CASE(7)
        CASE(8)  CASE(9)  CASE(10) CASE(11)
        CASE(12) CASE(13) CASE(14) CASE(15)
#undef CASE
    default:
        kpanic("pmpaddr index out of range");
    }
}

/* On RV32 each pmpcfg register holds four entry bytes. */
static uint32_t pmpcfg_read(unsigned reg)
{
    switch (reg) {
    case 0: return csr_read(pmpcfg0);
    case 1: return csr_read(pmpcfg1);
    case 2: return csr_read(pmpcfg2);
    case 3: return csr_read(pmpcfg3);
    default: kpanic("pmpcfg index out of range");
    }
}

static void pmpcfg_write(unsigned reg, uint32_t v)
{
    switch (reg) {
    case 0: csr_write(pmpcfg0, v); return;
    case 1: csr_write(pmpcfg1, v); return;
    case 2: csr_write(pmpcfg2, v); return;
    case 3: csr_write(pmpcfg3, v); return;
    default: kpanic("pmpcfg index out of range");
    }
}

static void pmpcfg_set_byte(unsigned idx, uint8_t cfg)
{
    unsigned reg = idx / 4;
    unsigned shift = (idx % 4) * 8;
    uint32_t v = pmpcfg_read(reg);
    v &= ~(0xffu << shift);
    v |= (uint32_t)cfg << shift;
    pmpcfg_write(reg, v);
}

static uint8_t pmpcfg_get_byte(unsigned idx)
{
    return (uint8_t)(pmpcfg_read(idx / 4) >> ((idx % 4) * 8));
}

/*
 * A pmpcfg byte as the board's CSRs lay it out, and back, since the swap undoes itself.
 * RP2350 transposes R and X, erratum RP2350-E6; see PMP_CFG_RX_TRANSPOSED in board.h.
 * Everything else in the kernel reads and writes the bytes as the specification has them.
 */
static uint8_t pmpcfg_board(uint8_t cfg)
{
#if PMP_CFG_RX_TRANSPOSED
    uint8_t r = cfg & PMP_R ? PMP_X : 0;
    uint8_t x = cfg & PMP_X ? PMP_R : 0;
    return (uint8_t)((cfg & ~(PMP_R | PMP_X)) | r | x);
#else
    return cfg;
#endif
}

/* pmpcfg_board for the four bytes of a register at once. */
static uint32_t pmpcfg_board_word(uint32_t cfg)
{
#if PMP_CFG_RX_TRANSPOSED
    const uint32_t r = 0x01010101u * PMP_R, x = 0x01010101u * PMP_X;
    _Static_assert(PMP_X == PMP_R << 2, "R and X lie two bits apart");
    return (cfg & ~(r | x)) | (cfg & r) << 2 | (cfg & x) >> 2;
#else
    return cfg;
#endif
}

unsigned pmp_entry_count;
unsigned pmp_entry_end;
uint32_t pmp_grain;

void pmp_init(void)
{
    for (unsigned reg = 0; reg < PMP_CSR_ENTRIES / 4; reg++) {
        pmpcfg_write(reg, 0);
    }

    /*
     * The grain, by the specification's recipe: with the mode OFF,
     * write all ones to pmpaddr0 and read back;
     * the lowest set bit is G and the grain is 2^(G+2) bytes.
     */
    pmpaddr_write(0, 0xffffffffu);
    uint32_t back = pmpaddr_read(0);
    pmpaddr_write(0, 0);
    if (back == 0) {
        kpanic("no PMP");
    }
    pmp_grain = 4;
    while ((back & 1u) == 0) {
        back >>= 1;
        pmp_grain <<= 1;
    }
    /* RP2350's core hides its grain from the recipe; see PMP_GRAIN_MIN in board.h. */
    if (pmp_grain < PMP_GRAIN_MIN) {
        pmp_grain = PMP_GRAIN_MIN;
    }

    /*
     * The image uses entries from zero up, so the budget ends at the first
     * entry whose mode, rights or address ignores a write.
     * NAPOT is the only mode the image uses, so it is the mode probed, with no rights.
     * The configuration goes first, since RP2350's hardwired entries hold theirs read only
     * but seem to take a write of their address; see DESIGN.md, "Physical Memory Protection".
     */
    unsigned count = 0;
    for (unsigned i = 0; i < PMP_MAX_ENTRIES; i++) {
        pmpcfg_set_byte(i, PMP_A_NAPOT);
        uint8_t cfg = pmpcfg_get_byte(i);
        pmpcfg_set_byte(i, 0);
        if (cfg != PMP_A_NAPOT) {
            break;
        }
        pmpaddr_write(i, 0xffffffffu);
        uint32_t ones = pmpaddr_read(i);
        pmpaddr_write(i, 0);
        uint32_t zero = pmpaddr_read(i);
        if (ones == zero) {
            break;
        }
        count++;
    }
    pmp_entry_count = count;

    /*
     * Every entry was turned off above, so one past the run still on is hardwired, as RP2350's three are.
     * They are looked for past the budget too, since a budget below them would leave them open.
     */
    pmp_entry_end = count;
    for (unsigned i = count; i < PMP_CSR_ENTRIES; i++) {
        uint8_t cfg = pmpcfg_get_byte(i);
        if ((cfg & 0x18) != PMP_A_OFF && (cfg & (PMP_R | PMP_W | PMP_X)) != 0) {
            pmp_entry_end = i + 1;
        }
    }
}

void pmp_set(unsigned idx, uint32_t addr, uint8_t cfg)
{
    /* Address first, then rights; harmless today since PMP is off in machine mode. */
    pmpaddr_write(idx, addr);
    pmpcfg_set_byte(idx, pmpcfg_board(cfg));
}

/*
 * The image's addresses go straight to their CSRs, from the last down, through one jump;
 * an entry turned off keeps its address, which matches nothing while its mode is OFF.
 * Its configuration goes a register at a time, four entries in each:
 * the image's bytes below count, zero up to off, and in the register that holds pmp_entry_count
 * the bytes from it up read back and kept, so the fence and what the core hardwires stay as they were.
 */
void pmp_load(const uint32_t *addr, const uint8_t *cfg, unsigned count, unsigned off)
{
    switch (count) {
#define CASE(n) case n + 1: csr_write(pmpaddr##n, addr[n]); __attribute__((fallthrough));
        CASE(15) CASE(14) CASE(13) CASE(12)
        CASE(11) CASE(10) CASE(9)  CASE(8)
        CASE(7)  CASE(6)  CASE(5)  CASE(4)
        CASE(3)  CASE(2)  CASE(1)  CASE(0)
#undef CASE
    case 0:
        break;
    default:
        kpanic("pmp_load count out of range");
    }
    const uint8_t *bytes = __builtin_assume_aligned(cfg, 4);
    unsigned end = count > off ? count : off;
    for (unsigned reg = 0; reg * 4 < end; reg++) {
        LOOP_BOUND((PMP_MAX_ENTRIES + 3) / 4);
        unsigned first = reg * 4;
        /* The bytes of entries below count, and those of entries from pmp_entry_count up. */
        uint32_t image = count >= first + 4 ? ~0u : count > first ? (1u << (8 * (count - first))) - 1 : 0;
        uint32_t kept = pmp_entry_count >= first + 4 ? 0 : ~0u << (8 * (pmp_entry_count - first));
        uint32_t v;
        __builtin_memcpy(&v, bytes + first, 4);
        v = pmpcfg_board_word(v) & image;
        if (kept != 0) {
            v |= pmpcfg_read(reg) & kept;
        }
        pmpcfg_write(reg, v);
    }
}

void pmp_get(unsigned idx, uint32_t *addr, uint8_t *cfg)
{
    *addr = pmpaddr_read(idx);
    *cfg = pmpcfg_board(pmpcfg_get_byte(idx));
}
