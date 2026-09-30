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
 */

#include <stdint.h>

#include "csr.h"
#include "irq.h"
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

void irq_enable(uint32_t line, bool on)
{
    if (on) {
        ARRAY_SET(MEIEA, BIT(line) | WINDOW(line));
    } else {
        ARRAY_CLEAR(MEIEA, BIT(line) | WINDOW(line));
    }
}

/* Mask every line, drop anything forced before, and take the machine external interrupt. */
void irq_init(void)
{
    for (uint32_t window = 0; window < WINDOWS; window++) {
        ARRAY_CLEAR(MEIEA, 0xffff0000u | window);
        ARRAY_CLEAR(MEIFA, 0xffff0000u | window);
    }
    csr_set(mie, MIE_MEIE);
}

bool irq_enabled(uint32_t line)
{
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
