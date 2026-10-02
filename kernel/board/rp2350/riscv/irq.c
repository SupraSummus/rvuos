/*
 * Interrupt controller of RP2350's Hazard3: the Xh3irq extension, in CSRs of its own.
 *
 * Every system IRQ is a level into the core, and three arrays of one bit per IRQ describe them:
 * meiea enables, meipa shows the levels, and meifa forces an IRQ pending from software.
 * Each is reached through a 16-bit window in the upper half of its CSR,
 * chosen by the low five bits written by the same instruction,
 * so a csrrs of a window's index reads it and one with bits above sets them.
 * The core raises the machine external interrupt while an enabled IRQ is pending,
 * on mcause 11, as a standard core does.
 * rvuos gives every IRQ the same priority, zero, and a line is an IRQ.
 * A claim finds the lowest line that is pending and enabled;
 * nothing is held between a claim and a completion,
 * since masking the line already lowers the machine external interrupt.
 *
 * Each core has these CSRs of its own, and every IRQ reaches both, but the device lines are the first core's;
 * see DESIGN.md, "Cores".
 * So a call on the second core that arms or disarms a line changes the lines the first is to enable, irq_wanted,
 * and interrupts it, and the first makes its meiea agree as it next takes the lock, as an NVIC does; see irq_sync.
 * meipa shows each level as it is, so nothing pending is left to drop.
 */

#include <stdint.h>

#include "csr.h"
#include "irq.h"
#include "kernel.h"
#include "object.h"
#include "work.h"

#define WINDOWS ((IRQ_LINES + 15) / 16)

/* The window of an array holding a line, and the line's bit in the window's half. */
#define WINDOW(line) ((line) / 16)
#define BIT(line)    (1u << (16 + (line) % 16))

#define MEIEA 0xbe0
#define MEIPA 0xbe1
#define MEIFA 0xbe2

#define CSR_NAME_(csr) #csr
#define CSR_NAME(csr)  CSR_NAME_(csr)

/* A window of an array, read by a csrrs that writes its index and sets nothing. */
#define ARRAY_READ(csr, window)                                                     \
    ({                                                                              \
        uint32_t v_;                                                                \
        __asm__ volatile("csrrs %0, " CSR_NAME(csr) ", %1" : "=r"(v_) : "r"(window)); \
        v_ >> 16;                                                                   \
    })

/* Set or clear the bits of the upper half in the window the lower half names. */
#define ARRAY_SET(csr, value)   __asm__ volatile("csrs " CSR_NAME(csr) ", %0" : : "r"(value))
#define ARRAY_CLEAR(csr, value) __asm__ volatile("csrc " CSR_NAME(csr) ", %0" : : "r"(value))

#if CORES > 1
/* The device lines the first core's meiea is to enable, a window's at a time, and whether it may not yet. */
static uint32_t irq_wanted[WINDOWS];
static bool irq_stale;
#endif

/* This core's meiea forwards a line, or stops it. */
static void meiea_enable(uint32_t line, bool on)
{
    if (on) {
        ARRAY_SET(MEIEA, BIT(line) | WINDOW(line));
    } else {
        ARRAY_CLEAR(MEIEA, BIT(line) | WINDOW(line));
    }
}

void irq_enable(uint32_t line, bool on)
{
#if CORES > 1
    if (on) {
        irq_wanted[WINDOW(line)] |= 1u << (line % 16);
    } else {
        irq_wanted[WINDOW(line)] &= ~(1u << (line % 16));
    }
    if (core_index() != 0) {
        irq_stale = true;
        core_interrupt_later(&cores[0]);
        return;
    }
#endif
    meiea_enable(line, on);
}

/* Mask every line of this core and drop anything forced before; the external interrupt stays as it was. */
void irq_core_init(void)
{
    for (uint32_t window = 0; window < WINDOWS; window++) {
        LOOP_BOUND(WINDOWS);
        ARRAY_CLEAR(MEIEA, 0xffff0000u | window);
        ARRAY_CLEAR(MEIFA, 0xffff0000u | window);
    }
}

/* And on the first core, take the machine external interrupt. */
void irq_init(void)
{
    irq_core_init();
    csr_set(mie, MIE_MEIE);
}

void irq_sync(void)
{
#if CORES > 1
    if (!irq_stale || core_index() != 0) {
        return;
    }
    irq_stale = false;
    for (uint32_t window = 0; window < WINDOWS; window++) {
        LOOP_BOUND(WINDOWS);
        uint32_t have = ARRAY_READ(MEIEA, window);
        uint32_t want = irq_wanted[window];
        ARRAY_CLEAR(MEIEA, (have & ~want) << 16 | window);
        ARRAY_SET(MEIEA, (want & ~have) << 16 | window);
    }
#endif
}

/* As the first core's meiea holds it, or will once that core has the lock; the second cannot read the first's. */
bool irq_enabled(uint32_t line)
{
#if CORES > 1
    if (core_index() != 0) {
        return (irq_wanted[WINDOW(line)] >> (line % 16) & 1u) != 0;
    }
#endif
    return (ARRAY_READ(MEIEA, WINDOW(line)) >> (line % 16) & 1u) != 0;
}

/* The lowest-numbered line that is pending and enabled; which of several comes first is not promised. */
uint32_t irq_claim(void)
{
    for (uint32_t window = 0; window < WINDOWS; window++) {
        LOOP_BOUND(WINDOWS);
        uint32_t high = ARRAY_READ(MEIPA, window) & ARRAY_READ(MEIEA, window);
        for (uint32_t bit = 0; high != 0 && bit < 16; bit++, high >>= 1) {
            LOOP_BOUND(16);
            uint32_t line = 16 * window + bit;
            if ((high & 1u) && line != 0 && line < IRQ_LINES) {
                return line;
            }
        }
    }
    return 0;
}

void irq_complete(uint32_t line)
{
    (void)line;
}
