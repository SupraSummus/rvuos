/*
 * The counter under the tick on nRF52840: TIMER0, counting microseconds in 32 bits.
 *
 * A TIMER keeps its count to itself; a CAPTURE task copies it into a CC register, which is all a read sees.
 * So TIMER1 raises its COMPARE[0] event every microsecond, clearing itself at each,
 * and PPI channel 0 makes every one of them TIMER0's CAPTURE[1]:
 * CC[1] is TIMER0's count at most a microsecond old, which a load reads,
 * the kernel here and user mode through OP_CLOCK_FRAME alike; see COUNTER_ADDR in board.h.
 *
 * It is 32 bits wide, so the kernel counts its wraps, as the MPS2 boards do with theirs:
 * see kernel/board/mps2/timer.c, whose argument holds here with one core.
 */

#include <stdint.h>

#include "layout.h"
#include "timer.h"

#define REG(addr) (*(volatile uint32_t *)(addr))

#define TIMER1_BASE 0x40009000u
#define TIMER_TASKS_START       0x000u
#define TIMER_TASKS_CAPTURE(n)  (0x040u + 4u * (n))
#define TIMER_EVENTS_COMPARE(n) (0x140u + 4u * (n))
#define TIMER_SHORTS            0x200u
#define TIMER_BITMODE           0x508u
#define TIMER_PRESCALER         0x510u
#define TIMER_CC(n)             (0x540u + 4u * (n))
#define TIMER_SHORTS_COMPARE0_CLEAR 1u
#define TIMER_BITMODE_32        3u
#define TIMER_PRESCALER_1MHZ    4u /* 16 MHz / 2^4 */

#define PPI_CHENSET   0x4001f504u
#define PPI_CH_EEP(n) (0x4001f510u + 8u * (n))
#define PPI_CH_TEP(n) (0x4001f514u + 8u * (n))

_Static_assert(COUNTER_ADDR == TIMER0_BASE + TIMER_CC(1), "the counter is TIMER0's CC[1]");
_Static_assert(COUNTER_HZ == 1000000u, "the timers count microseconds");
_Static_assert(COUNTER_HZ % TIMER_HZ == 0, "the tick divides the counter's second");
_Static_assert(COUNTER_HZ / TIMER_HZ <= TICK_COUNTS_MAX, "an account counts a tick of the counter");

/* The count's bits from 31 up, at least as far as any read has seen them. */
static uint32_t counter_upper;

uint64_t counter_read(void)
{
    uint64_t from = (uint64_t)counter_upper << 31;
    uint64_t now = from + (uint32_t)(REG(COUNTER_ADDR) - (uint32_t)from);
    counter_upper = (uint32_t)(now >> 31);
    return now;
}

/* Both timers as reset leaves them, in timer mode with their interrupts off. */
void timer_init(void)
{
    REG(TIMER0_BASE + TIMER_BITMODE) = TIMER_BITMODE_32;
    REG(TIMER0_BASE + TIMER_PRESCALER) = TIMER_PRESCALER_1MHZ;
    REG(TIMER1_BASE + TIMER_PRESCALER) = TIMER_PRESCALER_1MHZ;
    REG(TIMER1_BASE + TIMER_CC(0)) = 1;
    REG(TIMER1_BASE + TIMER_SHORTS) = TIMER_SHORTS_COMPARE0_CLEAR;
    REG(PPI_CH_EEP(0)) = TIMER1_BASE + TIMER_EVENTS_COMPARE(0);
    REG(PPI_CH_TEP(0)) = TIMER0_BASE + TIMER_TASKS_CAPTURE(1);
    REG(PPI_CHENSET) = 1u;
    REG(TIMER0_BASE + TIMER_TASKS_START) = 1;
    REG(TIMER1_BASE + TIMER_TASKS_START) = 1;
    timer_start(COUNTER_HZ / TIMER_HZ);
}
