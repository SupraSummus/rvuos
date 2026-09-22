/*
 * Interrupt controller of the QEMU virt board: the PLIC, as seen by hart 0 in machine mode.
 * A board with another controller replaces this file, as it does timer.c and halt.c.
 */

#include <stdint.h>

#include "csr.h"
#include "irq.h"

#define PLIC_BASE      0x0c000000u
#define PLIC_PRIORITY  (PLIC_BASE + 0x0000u)   /* one word per line */
#define PLIC_ENABLE    (PLIC_BASE + 0x2000u)   /* one bit per line, context 0 */
#define PLIC_THRESHOLD (PLIC_BASE + 0x200000u) /* context 0 */
#define PLIC_CLAIM     (PLIC_BASE + 0x200004u) /* context 0; read claims, write completes */

#define REG(addr) (*(volatile uint32_t *)(addr))

/* Every line at the same priority: which fires first when several are pending is not promised. */
#define LINE_PRIORITY 1

/*
 * The PLIC forwards a line while it is pending and enabled,
 * which real hardware computes continuously.
 * QEMU's model (8.2) recomputes on a claim, a completion, a level change
 * and a priority or threshold write, but not on an enable write,
 * so a line already pending when it is unmasked would wait for an unrelated event.
 * Writing the threshold back makes the model recompute at once.
 */
void irq_enable(uint32_t line, bool on)
{
    volatile uint32_t *word = &REG(PLIC_ENABLE + 4 * (line / 32));
    uint32_t bit = 1u << (line % 32);
    *word = on ? (*word | bit) : (*word & ~bit);
    REG(PLIC_THRESHOLD) = 0;
}

void irq_init(void)
{
    for (uint32_t word = 0; word < (IRQ_LINES + 31) / 32; word++) {
        REG(PLIC_ENABLE + 4 * word) = 0;
    }
    for (uint32_t line = 1; line < IRQ_LINES; line++) {
        REG(PLIC_PRIORITY + 4 * line) = LINE_PRIORITY;
    }
    REG(PLIC_THRESHOLD) = 0;
    csr_set(mie, MIE_MEIE);
}

bool irq_enabled(uint32_t line)
{
    return (REG(PLIC_ENABLE + 4 * (line / 32)) >> (line % 32)) & 1u;
}

uint32_t irq_claim(void)
{
    return REG(PLIC_CLAIM);
}

void irq_complete(uint32_t line)
{
    REG(PLIC_CLAIM) = line;
}
