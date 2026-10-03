#include "lib.h"

#include <stdarg.h>

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d < s) {
        while (n--) {
            *d++ = *s++;
        }
    } else {
        while (n--) {
            d[n] = s[n];
        }
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

static void put_hex(const struct out *o, uint32_t v, int digits)
{
    for (int shift = 4 * (digits - 1); shift >= 0; shift -= 4) {
        o->put(o->to, "0123456789abcdef"[(v >> shift) & 0xfu]);
    }
}

static void put_dec(const struct out *o, uint32_t v)
{
    char buf[10];
    int i = 0;
    do {
        buf[i++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (i) {
        o->put(o->to, buf[--i]);
    }
}

void say(const struct out *o, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%' || fmt[1] == '\0') {
            o->put(o->to, *fmt);
            continue;
        }
        switch (*++fmt) {
        case 's':
            for (const char *s = va_arg(ap, const char *); *s; s++) {
                o->put(o->to, *s);
            }
            break;
        case 'u':
            put_dec(o, va_arg(ap, uint32_t));
            break;
        case 'd': {
            int32_t v = va_arg(ap, int32_t);
            if (v < 0) {
                o->put(o->to, '-');
            }
            put_dec(o, v < 0 ? 0u - (uint32_t)v : (uint32_t)v);
            break;
        }
        case 'x':
            o->put(o->to, '0');
            o->put(o->to, 'x');
            put_hex(o, va_arg(ap, uint32_t), 8);
            break;
        case 'M': {
            const uint8_t *mac = va_arg(ap, const uint8_t *);
            for (int i = 0; i < 6; i++) {
                put_hex(o, mac[i], 2);
                if (i < 5) {
                    o->put(o->to, ':');
                }
            }
            break;
        }
        case 'I': {
            uint32_t ip = va_arg(ap, uint32_t);
            const uint8_t *b = (const uint8_t *)&ip;
            for (int i = 0; i < 4; i++) {
                put_dec(o, b[i]);
                if (i < 3) {
                    o->put(o->to, '.');
                }
            }
            break;
        }
        default:
            o->put(o->to, *fmt);
        }
    }
    va_end(ap);
}

int plog_put(struct plog *l, char c)
{
    uint32_t head = l->head;
    l->bytes[head % PLOG_SIZE] = c;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    l->head = head + 1u;
    return c == '\n';
}

uint32_t plog_take(const struct plog *l, uint32_t taken, void (*put)(char))
{
    uint32_t head = l->head;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (head - taken > PLOG_SIZE) {
        taken = head - PLOG_SIZE;
    }
    for (; taken != head; taken++) {
        put(l->bytes[taken % PLOG_SIZE]);
    }
    return taken;
}
