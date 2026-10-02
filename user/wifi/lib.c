#include "lib.h"

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
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

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

void print(const struct out *o, const char *s)
{
    while (*s) {
        o->put(o->to, *s++);
    }
}

void print_hex(const struct out *o, uint32_t v)
{
    print(o, "0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        o->put(o->to, "0123456789abcdef"[(v >> shift) & 0xfu]);
    }
}

void print_dec(const struct out *o, uint32_t v)
{
    char buf[11];
    int i = 10;
    buf[i] = '\0';
    do {
        buf[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    print(o, &buf[i]);
}
