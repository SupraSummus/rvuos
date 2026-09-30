#ifndef RVUOS_USER_CSRS_H
#define RVUOS_USER_CSRS_H

/* User mode on this board writes no core state beyond its registers; see kernel/board/mps2-an385/board.c. */

#include <stdbool.h>

static inline bool csrs_mark(void)
{
    return true;
}

static inline bool csrs_are_reset(void)
{
    return true;
}

#endif
