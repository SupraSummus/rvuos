/*
 * The compiler may emit calls to these even in freestanding code,
 * for struct initialisation and copies.
 */

#include <stdint.h>

#include "kernel.h"

/* A word the compiler lets alias whatever it is stored over. */
typedef uint32_t __attribute__((may_alias)) word;

void *memset(void *dst, int c, size_t n)
{
    /* A pool zeroes each object with interrupts off, and an object lies on OBJ_ALIGN and is whole OBJ_ALIGN long. */
    if ((((uintptr_t)dst | n) & (4 * sizeof(word) - 1u)) == 0) {
        word *w = dst;
        word v = (uint8_t)c * 0x01010101u;
        for (; n > 0; n -= 4 * sizeof(word)) {
            LOOP_ARG(memset, n);
            w[0] = v;
            w[1] = v;
            w[2] = v;
            w[3] = v;
            w += 4;
        }
        return dst;
    }
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
