/*
 * The protection image of pmp.h on the MPU: PMSAv7 on ARMv7-M, PMSAv8 on ARMv8-M.
 *
 * A PMSAv7 region is a block: a power of two of at least 32 bytes, aligned to its own size,
 * which is what a NAPOT entry describes, so the image keeps PMP's encoding
 * and each entry becomes one region here, entry i region i; see DESIGN.md, "Architectures".
 * A PMSAv8 region is a base and a limit, each on 32 bytes, which holds every such block too.
 * The kernel runs with the MPU off, as machine mode runs outside PMP,
 * and turns it on as it returns into a thread, with the default memory map behind the regions, PRIVDEFENA;
 * PMSAv8's regions bind privileged code too, and none lets the kernel write where the thread only reads.
 * A region takes its memory type from where it lies: normal memory in RAM, a device elsewhere.
 */

#include "arch.h"
#include "armv7m.h"
#include "kernel.h"
#include "layout.h"
#include "pmp.h"
#include "scs.h"

_Static_assert(PMP_GRAIN_MIN >= 32, "no MPU region is smaller than 32 bytes");

unsigned pmp_entry_count;
unsigned pmp_entry_end;
uint32_t pmp_grain;

#if !ARMV8M

#define RASR_ENABLE (1u << 0)
#define RASR_SIZE_SHIFT 1 /* the region is 2^(SIZE + 1) bytes */
#define RASR_B  (1u << 16)
#define RASR_C  (1u << 17)
#define RASR_S  (1u << 18)
#define RASR_AP_SHIFT 24
#define RASR_AP_MASK (7u << RASR_AP_SHIFT)
#define RASR_XN (1u << 28)

/* Access permissions: the kernel's, and user mode's. */
#define AP_KERNEL      1u /* read and write for the kernel, nothing for user mode */
#define AP_READ_ONLY   2u /* user mode reads */
#define AP_READ_WRITE  3u /* user mode reads and writes */

/* Normal memory, write-back, or a shared device. */
#define RASR_NORMAL (RASR_C | RASR_B)
#define RASR_DEVICE (RASR_S | RASR_B)

void pmp_set(unsigned idx, uint32_t addr, uint8_t cfg)
{
    uint64_t base, size;
    pmp_napot_range(addr, &base, &size);
    uint32_t rasr = RASR_ENABLE | ((uint32_t)(__builtin_ctzll(size) - 1) << RASR_SIZE_SHIFT);
    if (!(cfg & PMP_R)) {
        rasr |= AP_KERNEL << RASR_AP_SHIFT;
    } else if (cfg & PMP_W) {
        rasr |= AP_READ_WRITE << RASR_AP_SHIFT;
    } else {
        rasr |= AP_READ_ONLY << RASR_AP_SHIFT;
    }
    if (!(cfg & PMP_X)) {
        rasr |= RASR_XN;
    }
    rasr |= ram_contains((uint32_t)base, (uint32_t)size) ? RASR_NORMAL : RASR_DEVICE;
    SCS_REG(MPU_RNR) = idx;
    SCS_REG(MPU_RBAR) = (uint32_t)base;
    SCS_REG(MPU_RASR) = rasr;
}

/* A region read back as the entry that would have made it; a disabled one is an entry that is off. */
void pmp_get(unsigned idx, uint32_t *addr, uint8_t *cfg)
{
    SCS_REG(MPU_RNR) = idx;
    uint32_t rasr = SCS_REG(MPU_RASR);
    if (!(rasr & RASR_ENABLE)) {
        *addr = 0;
        *cfg = 0;
        return;
    }
    uint32_t base = SCS_REG(MPU_RBAR) & ~31u;
    uint32_t size = 1u << (((rasr >> RASR_SIZE_SHIFT) & 0x1fu) + 1);
    uint32_t ap = (rasr & RASR_AP_MASK) >> RASR_AP_SHIFT;
    uint8_t c = PMP_A_NAPOT;
    if (ap == AP_READ_ONLY || ap == AP_READ_WRITE || ap == 6u || ap == 7u) {
        c |= PMP_R;
    }
    if (ap == AP_READ_WRITE) {
        c |= PMP_W;
    }
    if (!(rasr & RASR_XN)) {
        c |= PMP_X;
    }
    *addr = pmp_napot_addr(base, size);
    *cfg = c;
}

#else

#define RBAR_XN (1u << 0)
#define RBAR_AP_SHIFT 1
#define RBAR_AP_MASK (3u << RBAR_AP_SHIFT)
#define RLAR_ENABLE (1u << 0)
#define RLAR_ATTR_SHIFT 1

/* Access permissions, for the kernel as for user mode alike but the first. */
#define AP_KERNEL     0u /* read and write for the kernel, nothing for user mode */
#define AP_READ_WRITE 1u
#define AP_READ_ONLY  3u

/* The memory types, as indices into MAIR0: normal memory, write-back, and a device, nGnRnE. */
#define ATTR_NORMAL 0u
#define ATTR_DEVICE 1u
#define MAIR0_VALUE (0xffu << (8 * ATTR_NORMAL) | 0x00u << (8 * ATTR_DEVICE))

void pmp_set(unsigned idx, uint32_t addr, uint8_t cfg)
{
    uint64_t base, size;
    pmp_napot_range(addr, &base, &size);
    uint32_t rbar = (uint32_t)base;
    if (!(cfg & PMP_R)) {
        rbar |= AP_KERNEL << RBAR_AP_SHIFT;
    } else if (cfg & PMP_W) {
        rbar |= AP_READ_WRITE << RBAR_AP_SHIFT;
    } else {
        rbar |= AP_READ_ONLY << RBAR_AP_SHIFT;
    }
    if (!(cfg & PMP_X)) {
        rbar |= RBAR_XN;
    }
    uint32_t attr = ram_contains((uint32_t)base, (uint32_t)size) ? ATTR_NORMAL : ATTR_DEVICE;
    SCS_REG(MPU_RNR) = idx;
    SCS_REG(MPU_RBAR) = rbar;
    SCS_REG(MPU_RLAR) = (uint32_t)(base + size - 32u) | attr << RLAR_ATTR_SHIFT | RLAR_ENABLE;
}

/* A region read back as the entry that would have made it; a disabled one is an entry that is off. */
void pmp_get(unsigned idx, uint32_t *addr, uint8_t *cfg)
{
    SCS_REG(MPU_RNR) = idx;
    uint32_t rlar = SCS_REG(MPU_RLAR);
    if (!(rlar & RLAR_ENABLE)) {
        *addr = 0;
        *cfg = 0;
        return;
    }
    uint32_t rbar = SCS_REG(MPU_RBAR);
    uint32_t base = rbar & ~31u;
    uint32_t size = (rlar & ~31u) + 32u - base;
    uint32_t ap = (rbar & RBAR_AP_MASK) >> RBAR_AP_SHIFT;
    uint8_t c = PMP_A_NAPOT;
    if (ap == AP_READ_ONLY || ap == AP_READ_WRITE) {
        c |= PMP_R;
    }
    if (ap == AP_READ_WRITE) {
        c |= PMP_W;
    }
    if (!(rbar & RBAR_XN)) {
        c |= PMP_X;
    }
    *addr = pmp_napot_addr(base, size);
    *cfg = c;
}

#endif

static void mpu_sync(void)
{
    __asm__ volatile("dsb\n\tisb" : : : "memory");
}

/* Every region cleared, and the MPU off until the first thread runs; see mpu_thread. */
void pmp_init(void)
{
    mpu_kernel();
    unsigned regions = (SCS_REG(MPU_TYPE) >> 8) & 0xffu;
    if (regions == 0) {
        kpanic("no MPU");
    }
    for (unsigned i = 0; i < regions; i++) {
        pmp_clear(i);
    }
    pmp_entry_count = regions < PMP_MAX_ENTRIES ? regions : PMP_MAX_ENTRIES;
    /* The default memory map lets unprivileged code in nowhere, so nothing past the regions grants a thread a right. */
    pmp_entry_end = pmp_entry_count;
    /* The MPU has no finer grain to probe for; its smallest region is the board's minimum. */
    pmp_grain = PMP_GRAIN_MIN;
#if ARMV8M
    SCS_REG(MPU_MAIR0) = MAIR0_VALUE;
#endif
}

/* The kernel writes the regions only while the MPU is off, and mpu_thread's barrier makes them take. */
void pmp_clear(unsigned idx)
{
    SCS_REG(MPU_RNR) = idx;
    SCS_REG(MPU_RASR) = 0;
    SCS_REG(MPU_RBAR) = 0;
}

void mpu_kernel(void)
{
    SCS_REG(MPU_CTRL) = 0;
    mpu_sync();
}

void mpu_thread(void)
{
    SCS_REG(MPU_CTRL) = MPU_CTRL_ENABLE | MPU_CTRL_PRIVDEFENA;
    mpu_sync();
}
