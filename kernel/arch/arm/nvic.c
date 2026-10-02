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
 *
 * Each core has an NVIC of its own, which only that core reaches, and the device lines are the first core's;
 * see DESIGN.md, "Cores".
 * So a call on another core that arms or disarms a line changes the lines the first core is to enable, nvic_wanted,
 * and interrupts it; the first core makes its NVIC agree as it next takes the lock, see nvic_sync.
 * Until then it may enter on a line no Irq is armed on any more, which irq_claim then passes over.
 * IPI_LINE, the line the cores interrupt each other on where the board has one, is the kernel's on every core,
 * and no device's.
 */

#include <stdint.h>

#include "armv7m.h"
#include "irq.h"
#include "kernel.h"
#include "object.h"
#include "scs.h"
#include "work.h"

#define WORDS ((IRQ_LINES + 31) / 32)
#define WORD(line) (4u * ((line) / 32))
#define BIT(line)  (1u << ((line) % 32))

uint32_t nvic_entered;

#if CORES > 1
_Static_assert(IPI_LINE > 0 && IPI_LINE < IRQ_LINES, "the cores' line is one of the controller's");

/* The device lines the first core's NVIC is to enable, and whether it may not yet. */
static uint32_t nvic_wanted[WORDS];
static bool nvic_stale;

/* The lines of a word that are the kernel's on every core, no device's. */
#define KERNEL_LINES(w) ((w) == IPI_LINE / 32 ? BIT(IPI_LINE) : 0u)
#else
#define KERNEL_LINES(w) 0u
#endif

/* The device lines of this core's NVIC that are pending and enabled, a word of them. */
static uint32_t nvic_high(uint32_t w)
{
    return SCS_REG(NVIC_ISPR + 4 * w) & SCS_REG(NVIC_ISER + 4 * w) & ~KERNEL_LINES(w);
}

/* This core's NVIC with every line masked, none pending, and every line at the kernel's priority. */
void irq_core_init(void)
{
    for (uint32_t w = 0; w < WORDS; w++) {
        LOOP_BOUND(WORDS);
        SCS_REG(NVIC_ICER + 4 * w) = ~0u;
        SCS_REG(NVIC_ICPR + 4 * w) = ~0u;
    }
    for (uint32_t line = 0; line < IRQ_LINES; line++) {
        LOOP_BOUND(IRQ_LINES);
        ((volatile uint8_t *)NVIC_IPR)[line] = 0;
    }
}

void irq_init(void)
{
    irq_core_init();
}

/* This core's NVIC forwards a line, or stops it. */
static void nvic_enable(uint32_t line, bool on)
{
    if (on) {
        SCS_REG(NVIC_ICPR + WORD(line)) = BIT(line);
        SCS_REG(NVIC_ISER + WORD(line)) = BIT(line);
    } else {
        SCS_REG(NVIC_ICER + WORD(line)) = BIT(line);
    }
}

void irq_enable(uint32_t line, bool on)
{
#if CORES > 1
    if (on) {
        nvic_wanted[line / 32] |= BIT(line);
    } else {
        nvic_wanted[line / 32] &= ~BIT(line);
    }
    if (core_index() != 0) {
        nvic_stale = true;
        core_interrupt_later(&cores[0]);
        return;
    }
#endif
    nvic_enable(line, on);
}

void nvic_sync(void)
{
#if CORES > 1
    if (!nvic_stale || core_index() != 0) {
        return;
    }
    nvic_stale = false;
    for (uint32_t w = 0; w < WORDS; w++) {
        LOOP_BOUND(WORDS);
        uint32_t have = SCS_REG(NVIC_ISER + 4 * w) & ~KERNEL_LINES(w);
        uint32_t want = nvic_wanted[w];
        SCS_REG(NVIC_ICER + 4 * w) = have & ~want;
        SCS_REG(NVIC_ICPR + 4 * w) = want & ~have;
        SCS_REG(NVIC_ISER + 4 * w) = want & ~have;
    }
#endif
}

/* As the first core's NVIC holds it, or will once that core has the lock; another core cannot read that NVIC. */
bool irq_enabled(uint32_t line)
{
#if CORES > 1
    if (core_index() != 0) {
        return (nvic_wanted[line / 32] & BIT(line)) != 0;
    }
#endif
    return (SCS_REG(NVIC_ISER + WORD(line)) & BIT(line)) != 0;
}

/*
 * The line the trap entered on, while this core still forwards it, then the lowest line pending and enabled;
 * which of several comes first is not promised.
 */
uint32_t irq_claim(void)
{
    uint32_t line = nvic_entered;
    nvic_entered = 0;
    if (line != 0 && (SCS_REG(NVIC_ISER + WORD(line)) & BIT(line)) != 0) {
        return line;
    }
    for (uint32_t w = 0; w < WORDS; w++) {
        LOOP_BOUND(WORDS);
        uint32_t high = nvic_high(w);
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
        high |= nvic_high(w);
    }
    return high != 0;
}

bool nvic_ipi_pending(void)
{
#if CORES > 1
    return (SCS_REG(NVIC_ISPR + WORD(IPI_LINE)) & BIT(IPI_LINE)) != 0;
#else
    return false;
#endif
}
