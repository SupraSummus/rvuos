#ifndef RVUOS_USER_CSRS_H
#define RVUOS_USER_CSRS_H

/* QEMU has no CSR user mode writes that rvuos knows of; see kernel/board/qemu/board.c. */

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
