/*
 * The nRF52840's router, the PPI: channel n triggers the task register CH[n].TEP names
 * when the event register CH[n].EEP names is raised, while CHEN's bit n is set.
 * The kernel lends channels ROUTE_FIRST on, and keeps channel 0 for its counter, see timer.c;
 * the fork of each channel it lends stays off, as do the groups, which no frame reaches.
 * See route.h and DESIGN.md, "Routes".
 */

#include "route.h"

#define REG(off) (*(volatile uint32_t *)(PPI_BASE + (off)))

#define PPI_CHEN      0x500u
#define PPI_CHENSET   0x504u
#define PPI_CHENCLR   0x508u
#define PPI_EEP(n)    (0x510u + 8u * (n))
#define PPI_TEP(n)    (0x514u + 8u * (n))
#define PPI_FORK(n)   (0x910u + 4u * (n))

/* The APB's devices, a 4 KiB block each, where a device's tasks and events lie. */
#define APB_BASE      0x40000000u
#define APB_SIZE      0x00040000u
#define BLOCK_MASK    0xfffu
#define TASKS_FIRST   0x000u
#define TASKS_END     0x080u
#define EVENTS_FIRST  0x100u
#define EVENTS_END    0x180u

/*
 * A register a frame holds, word aligned, in a device of the APB, among its tasks or its events as the end asks,
 * and none of the kernel's own devices, TIMER0, TIMER1 and the PPI, whatever frame reaches them, the counter's does.
 */
bool router_end(uint32_t end, uint32_t base, uint32_t size, uint32_t name, uint32_t *word)
{
    uint32_t at = base + name;
    uint32_t off = at & BLOCK_MASK;
    uint32_t block = at & ~BLOCK_MASK;
    bool task = end == ROUTE_TASK;
    if (name % 4 != 0 || name >= size || at - APB_BASE >= APB_SIZE ||
        block == TIMER0_BASE || block == TIMER1_BASE || block == PPI_BASE ||
        (task ? off - TASKS_FIRST >= TASKS_END - TASKS_FIRST : off - EVENTS_FIRST >= EVENTS_END - EVENTS_FIRST)) {
        return false;
    }
    *word = at;
    return true;
}

/* The channel is off while its ends change, so it never wires the one end to the other's old partner. */
void router_connect(uint32_t channel, uint32_t event, uint32_t task)
{
    REG(PPI_CHENCLR) = 1u << channel;
    REG(PPI_EEP(channel)) = event;
    REG(PPI_TEP(channel)) = task;
    REG(PPI_CHENSET) = 1u << channel;
}

void router_disconnect(uint32_t channel)
{
    REG(PPI_CHENCLR) = 1u << channel;
}

bool router_connected(uint32_t channel, uint32_t *event, uint32_t *task)
{
    *event = REG(PPI_EEP(channel));
    *task = REG(PPI_TEP(channel));
    return (REG(PPI_CHEN) >> channel) & 1u;
}

void router_init(void)
{
    for (uint32_t c = ROUTE_FIRST; c < ROUTE_FIRST + ROUTE_CHANNELS; c++) {
        LOOP_BOUND(ROUTE_CHANNELS);
        REG(PPI_CHENCLR) = 1u << c;
        REG(PPI_EEP(c)) = 0;
        REG(PPI_TEP(c)) = 0;
        REG(PPI_FORK(c)) = 0;
    }
}
