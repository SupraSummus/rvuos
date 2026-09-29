#ifndef RVUOS_HOST_PADDR_H
#define RVUOS_HOST_PADDR_H

/*
 * Physical address translation for the host build.
 * Physical RAM is a buffer allocated by the harness;
 * an address outside it is a kernel bug, which breaks "Memory safety" in DESIGN.md,
 * and host/shim.c reports it on the spot as any invariant.
 */

#include <sanitizer/asan_interface.h>
#include <stdint.h>
#include <stdlib.h>

#define HOST_RAM_BASE 0x80000000u
#define HOST_RAM_SIZE 0x00800000u

extern uint8_t *host_ram;

__attribute__((noreturn)) void host_outside_ram(uint32_t p);

static inline void *p2v(uint32_t p)
{
    if (p < HOST_RAM_BASE || p - HOST_RAM_BASE >= HOST_RAM_SIZE) {
        host_outside_ram(p);
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

/*
 * ASan's poison lies on the RAM the kernel may not touch: all of it but its objects and its log.
 * host_boot poisons all but the log, the pools move the poison as objects come and go,
 * and host/shim.c reports the kernel reaching poison as it reports any invariant.
 */
static inline void ram_unpoison(const void *v, uint32_t size)
{
    ASAN_UNPOISON_MEMORY_REGION(v, size);
}

static inline void ram_poison(const void *v, uint32_t size)
{
    ASAN_POISON_MEMORY_REGION(v, size);
}

#endif
