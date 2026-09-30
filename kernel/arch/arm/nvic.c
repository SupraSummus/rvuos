/*
 * The interrupt controller every Cortex-M has, the NVIC: a line is an external interrupt, IRQ n.
 *
 * Every line has the priority the kernel runs at, zero, so the kernel takes one only from user mode,
 * where it enters on the line's own exception; see start.S.
 * The line it entered on is active from then on, and no longer pending,
 * so trap.c names it in nvic_entered and irq_claim hands it out first;
 * any other line the kernel finds pending and enabled it claims by looking,
 * which is also how it hears of lines while it waits in intr_wait.
 * Nothing is held between a claim and a completion, since masking the line already keeps it from being taken.
 *
 * A line becomes pending while its level is high and it is not active, masked or not,
 * so a line the driver serviced after the kernel masked it may still be pending;
 * unmasking drops that before it enables the line, and a level still high is pending again at once.
 */

#include <stdint.h>

#include "armv7m.h"
#include "irq.h"
#include "scs.h"
#include "work.h"

#define WORDS ((IRQ_LINES + 31) / 32)
#define WORD(line) (4u * ((line) / 32))
#define BIT(line)  (1u << ((line) % 32))

uint32_t nvic_entered;

void irq_init(void)
{
    for (uint32_t w = 0; w < WORDS; w++) {
        SCS_REG(NVIC_ICER + 4 * w) = ~0u;
        SCS_REG(NVIC_ICPR + 4 * w) = ~0u;
    }
    for (uint32_t line = 0; line < IRQ_LINES; line++) {
        ((volatile uint8_t *)NVIC_IPR)[line] = 0;
    }
}

void irq_enable(uint32_t line, bool on)
{
    if (on) {
        SCS_REG(NVIC_ICPR + WORD(line)) = BIT(line);
        SCS_REG(NVIC_ISER + WORD(line)) = BIT(line);
    } else {
        SCS_REG(NVIC_ICER + WORD(line)) = BIT(line);
    }
}

bool irq_enabled(uint32_t line)
{
    return (SCS_REG(NVIC_ISER + WORD(line)) & BIT(line)) != 0;
}

/* The line the trap entered on, then the lowest line pending and enabled; which of several comes first is not promised. */
uint32_t irq_claim(void)
{
    uint32_t line = nvic_entered;
    if (line != 0) {
        nvic_entered = 0;
        return line;
    }
    for (uint32_t w = 0; w < WORDS; w++) {
        LOOP_BOUND(WORDS);
        uint32_t high = SCS_REG(NVIC_ISPR + 4 * w) & SCS_REG(NVIC_ISER + 4 * w);
        for (uint32_t bit = 0; high != 0 && bit < 32; bit++, high >>= 1) {
            LOOP_BOUND(32);
            line = 32 * w + bit;
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

bool nvic_pending(void)
{
    uint32_t high = 0;
    for (uint32_t w = 0; w < WORDS; w++) {
        LOOP_BOUND(WORDS);
        high |= SCS_REG(NVIC_ISPR + 4 * w) & SCS_REG(NVIC_ISER + 4 * w);
    }
    return high != 0;
}
