#ifndef RVUOS_USER_CSRS_H
#define RVUOS_USER_CSRS_H

/* User mode on the MPS2 boards writes no core state beyond its registers; see their board.c. */

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
