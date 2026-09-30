#ifndef RVUOS_USER_CSRS_H
#define RVUOS_USER_CSRS_H

/* Hazard3 has no CSR user mode writes that rvuos knows of; see kernel/board/rp2350/board.c. */

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
