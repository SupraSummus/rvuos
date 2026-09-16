#ifndef RVUOS_HOST_PADDR_H
#define RVUOS_HOST_PADDR_H

/*
 * Physical address translation for the host build.
 * Physical RAM is a buffer allocated by the harness;
 * an address outside it is a kernel bug and aborts on the spot.
 */

#include <stdint.h>
#include <stdlib.h>

#define HOST_RAM_BASE 0x80000000u
#define HOST_RAM_SIZE 0x00800000u

extern uint8_t *host_ram;

static inline void *p2v(uint32_t p)
{
    if (p < HOST_RAM_BASE || p - HOST_RAM_BASE >= HOST_RAM_SIZE) {
        abort();
    }
    return host_ram + (p - HOST_RAM_BASE);
}

static inline uint32_t v2p(const void *v)
{
    const uint8_t *b = v;
    if (b < host_ram || (size_t)(b - host_ram) >= HOST_RAM_SIZE) {
        abort();
    }
    return HOST_RAM_BASE + (uint32_t)(b - host_ram);
}

#endif
