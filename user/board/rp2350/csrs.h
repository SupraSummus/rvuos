#ifndef RVUOS_USER_CSRS_H
#define RVUOS_USER_CSRS_H

/*
 * Hazard3 has no CSR user mode writes that rvuos knows of,
 * and the Cortex-M33's thread mode none beyond its registers; see kernel/board/rp2350/board.c.
 */

#include <stdbool.h>
#include <stdint.h>

static inline bool csrs_mark(void)
{
    return true;
}

static inline bool csrs_are_reset(void)
{
    return true;
}

/* Nothing to start, and nothing to keep. */
static inline uint32_t counter_start(void)
{
    return 0;
}

static inline bool counter_kept(uint32_t from)
{
    (void)from;
    return true;
}

#endif
