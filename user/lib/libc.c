#include "lib/libc.h"

#include <stdint.h>

/*
 * Whole aligned words where both ends allow, bytes for the rest, as newlib's do:
 * a 32-bit device register takes a byte store as a store of the whole word, its other bytes zero,
 * and programs copy into registers with these, Espressif's Wi-Fi libraries the MAC's keys.
 */
typedef uint32_t __attribute__((may_alias)) word;

static int aligned(uintptr_t a)
{
    return (a & (sizeof(word) - 1)) == 0;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (aligned((uintptr_t)d | (uintptr_t)s)) {
        for (; n >= sizeof(word); n -= sizeof(word), d += sizeof(word), s += sizeof(word)) {
            *(word *)d = *(const word *)s;
        }
    }
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

/* Forward, as memcpy copies, onto a destination below the source; backward onto one above it. */
void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d < s) {
        return memcpy(dst, src, n);
    }
    if (aligned((uintptr_t)(d + n) | (uintptr_t)(s + n))) {
        for (; n >= sizeof(word); n -= sizeof(word)) {
            *(word *)(d + n - sizeof(word)) = *(const word *)(s + n - sizeof(word));
        }
    }
    while (n--) {
        d[n] = s[n];
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    if (aligned((uintptr_t)d)) {
        word w = (uint8_t)c * 0x01010101u;
        for (; n >= sizeof(word); n -= sizeof(word), d += sizeof(word)) {
            *(word *)d = w;
        }
    }
    while (n--) {
        *d++ = (uint8_t)c;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    for (; n; n--, x++, y++) {
        if (*x != *y) {
            return *x - *y;
        }
    }
    return 0;
}

#ifdef __ARM_EABI__
/* What the ARM EABI has the compiler call where RISC-V's calls memset and memcpy, for struct initialisers and copies. */
void __aeabi_memclr(void *dst, size_t n);
void __aeabi_memclr4(void *dst, size_t n);
void __aeabi_memclr8(void *dst, size_t n);
void __aeabi_memcpy(void *dst, const void *src, size_t n);
void __aeabi_memcpy4(void *dst, const void *src, size_t n);
void __aeabi_memcpy8(void *dst, const void *src, size_t n);

void __aeabi_memclr(void *dst, size_t n) { memset(dst, 0, n); }
void __aeabi_memclr4(void *dst, size_t n) { memset(dst, 0, n); }
void __aeabi_memclr8(void *dst, size_t n) { memset(dst, 0, n); }
void __aeabi_memcpy(void *dst, const void *src, size_t n) { memcpy(dst, src, n); }
void __aeabi_memcpy4(void *dst, const void *src, size_t n) { memcpy(dst, src, n); }
void __aeabi_memcpy8(void *dst, const void *src, size_t n) { memcpy(dst, src, n); }
#endif

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}
