/*
 * The block fills and copies the ARM EABI has the compiler call where RISC-V's calls memset and memcpy,
 * for struct initialisation and copies; each is bounded by its size, as those are,
 * so every call says what bounds it with the CALL_ annotation that stood before it for RISC-V.
 * The variants that promise alignment are the same loops, each a function of its own,
 * so that the image names each by the function its loop stands in.
 */

#include <stdint.h>

#include "kernel.h"

void __aeabi_memclr(void *dst, size_t n);
void __aeabi_memcpy(void *dst, const void *src, size_t n);
void __aeabi_memclr4(void *dst, size_t n);
void __aeabi_memcpy4(void *dst, const void *src, size_t n);
void __aeabi_memclr8(void *dst, size_t n);
void __aeabi_memcpy8(void *dst, const void *src, size_t n);

void __aeabi_memclr(void *dst, size_t n)
{
    uint8_t *d = dst;
    while (n-- > 0) {
        LOOP_ARG(__aeabi_memclr, n);
        *d++ = 0;
    }
}

void __aeabi_memclr4(void *dst, size_t n)
{
    uint8_t *d = dst;
    while (n-- > 0) {
        LOOP_ARG(__aeabi_memclr4, n);
        *d++ = 0;
    }
}

void __aeabi_memclr8(void *dst, size_t n)
{
    uint8_t *d = dst;
    while (n-- > 0) {
        LOOP_ARG(__aeabi_memclr8, n);
        *d++ = 0;
    }
}

void __aeabi_memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n-- > 0) {
        LOOP_ARG(__aeabi_memcpy, n);
        *d++ = *s++;
    }
}

void __aeabi_memcpy4(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n-- > 0) {
        LOOP_ARG(__aeabi_memcpy4, n);
        *d++ = *s++;
    }
}

void __aeabi_memcpy8(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n-- > 0) {
        LOOP_ARG(__aeabi_memcpy8, n);
        *d++ = *s++;
    }
}
