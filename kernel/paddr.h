#ifndef RVUOS_PADDR_H
#define RVUOS_PADDR_H

#include <stdint.h>

/*
 * Physical addresses.
 *
 * The kernel stores physical addresses in capabilities and pool descriptors
 * and dereferences them through p2v().
 * On the target the two are the same thing.
 * In the host build, used for unit tests and fuzzing,
 * physical addresses index a simulated RAM buffer instead,
 * so the same kernel logic runs on a 64-bit machine.
 */
typedef uint32_t paddr_t;

#ifdef RVUOS_HOST
#include "host_paddr.h"
#else
static inline void *p2v(paddr_t p)
{
    return (void *)p;
}

static inline paddr_t v2p(const void *v)
{
    return (paddr_t)v;
}
#endif

#endif
