/*
 * The compiler may emit calls to these even in freestanding code,
 * for struct initialisation and copies.
 */

#include <stdint.h>

#include "kernel.h"

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    while (n-- > 0) {
        LOOP_ARG(memset, n);
        *d++ = (uint8_t)c;
    }
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n-- > 0) {
        LOOP_ARG(memcpy, n);
        *d++ = *s++;
    }
    return dst;
}
