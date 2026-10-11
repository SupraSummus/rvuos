/*
 * Alarms on a TIMER of the nRF52840's; see alarm.h.
 * Its registers are the Product Specification's, in the chapter on TIMER:
 * a count that a task captures into a compare register, and an event, with its interrupt, when it equals one.
 * TIMER2, the one the boot grants, has four compare registers.
 */

#include "alarm.h"

#define REG(a, off) (*(volatile uint32_t *)((a)->base + (off)))

#define TASKS_START   0x000u
#define TASKS_STOP    0x004u
#define TASKS_CLEAR   0x00cu
#define TASKS_CAPTURE(n)  (0x040u + 4u * (n))
#define EVENTS_COMPARE(n) (0x140u + 4u * (n))
#define SHORTS        0x200u
#define INTENSET      0x304u
#define INTENCLR      0x308u
#define MODE          0x504u
#define BITMODE       0x508u
#define PRESCALER     0x510u
#define CC(n)         (0x540u + 4u * (n))

#define INT_COMPARE(n) (1u << (16u + (n)))
#define MODE_TIMER     0u
#define BITMODE_32     3u
/* The TIMER counts 16 MHz divided by 2 to the prescaler. */
#define PRESCALER_1MHZ 4u
/* The compare register the count is captured into. */
#define NOW ALARMS

void alarm_init(struct alarm *a, uint32_t base)
{
    a->base = base;
    REG(a, TASKS_STOP) = 1;
    REG(a, INTENCLR) = ~0u;
    REG(a, SHORTS) = 0;
    REG(a, MODE) = MODE_TIMER;
    REG(a, BITMODE) = BITMODE_32;
    REG(a, PRESCALER) = PRESCALER_1MHZ;
    for (uint32_t n = 0; n < ALARMS; n++) {
        REG(a, EVENTS_COMPARE(n)) = 0;
    }
    REG(a, TASKS_CLEAR) = 1;
    REG(a, TASKS_START) = 1;
}

/* Whether the count t is still before at, as differences of the wrapping count tell. */
static bool before(uint32_t t, uint32_t at)
{
    return (int32_t)(at - t) > 0;
}

uint32_t alarm_now(struct alarm *a)
{
    REG(a, TASKS_CAPTURE(NOW)) = 1;
    return REG(a, CC(NOW));
}

bool alarm_set(struct alarm *a, uint32_t n, uint32_t at)
{
    REG(a, CC(n)) = at;
    REG(a, EVENTS_COMPARE(n)) = 0;
    REG(a, INTENSET) = INT_COMPARE(n);
    /* The count equals at once only: one already past it would reach it again in 71 minutes. */
    if (before(alarm_now(a), at)) {
        return true;
    }
    alarm_off(a, n);
    return false;
}

void alarm_off(struct alarm *a, uint32_t n)
{
    REG(a, INTENCLR) = INT_COMPARE(n);
    REG(a, EVENTS_COMPARE(n)) = 0;
}

bool alarm_fired(struct alarm *a, uint32_t n)
{
    if (REG(a, EVENTS_COMPARE(n)) == 0 || (REG(a, INTENSET) & INT_COMPARE(n)) == 0) {
        return false;
    }
    alarm_off(a, n);
    return true;
}
