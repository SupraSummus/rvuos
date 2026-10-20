/*
 * What the driver's own code and Espressif's libphy.a take from the rest of ESP-IDF and its C library, by name:
 * a little of libc, and formatted text for the logs.
 */

#include <stdarg.h>
#include <stdint.h>

#include "lib/libc.h"
#include "osi.h"

/* --- libc, beside user/lib/libc.c's. --- */

int strncmp(const char *a, const char *b, size_t n);
int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t size, const char *fmt, ...);

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n > 0; n--, a++, b++) {
        if (*a != *b || *a == 0) {
            return (unsigned char)*a - (unsigned char)*b;
        }
    }
    return 0;
}

/*
 * printf's conversions, enough for the logs:
 * flags - 0 and space, a width and a precision, given or *, the lengths hh h l ll z,
 * and d i u x X o p s c %.
 */
struct sink {
    char *buf;
    size_t size, len;
};

static void put(struct sink *k, char c)
{
    if (k->len + 1 < k->size) {
        k->buf[k->len] = c;
    }
    k->len++;
}

static void put_number(struct sink *k, unsigned long long v, int neg, unsigned base, int upper, int width, int prec,
                       int left, char pad, char sign)
{
    char digits[24];
    int n = 0;
    const char *set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do {
        digits[n++] = set[v % base];
        v /= base;
    } while (v);
    while (n < prec) {
        digits[n++] = '0';
    }
    char s = neg ? '-' : sign;
    int len = n + (s != 0);
    if (!left && pad == ' ') {
        for (; width > len; width--) {
            put(k, ' ');
        }
    }
    if (s) {
        put(k, s);
    }
    if (!left && pad == '0') {
        for (; width > len; width--) {
            put(k, '0');
        }
    }
    while (n) {
        put(k, digits[--n]);
    }
    for (; left && width > len; width--) {
        put(k, ' ');
    }
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list args)
{
    struct sink k = { buf, size, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            put(&k, *fmt);
            continue;
        }
        int left = 0, width = 0, prec = -1, lng = 0;
        char pad = ' ', sign = 0;
        for (fmt++;; fmt++) {
            if (*fmt == '-') {
                left = 1;
            } else if (*fmt == '0') {
                pad = '0';
            } else if (*fmt == ' ' || *fmt == '+') {
                sign = *fmt;
            } else if (*fmt != '#') {
                break;
            }
        }
        if (*fmt == '*') {
            width = va_arg(args, int);
            fmt++;
        }
        for (; *fmt >= '0' && *fmt <= '9'; fmt++) {
            width = width * 10 + (*fmt - '0');
        }
        if (*fmt == '.') {
            prec = 0;
            if (*++fmt == '*') {
                prec = va_arg(args, int);
                fmt++;
            }
            for (; *fmt >= '0' && *fmt <= '9'; fmt++) {
                prec = prec * 10 + (*fmt - '0');
            }
        }
        for (; *fmt == 'l' || *fmt == 'h' || *fmt == 'z' || *fmt == 'j' || *fmt == 't'; fmt++) {
            lng += *fmt == 'l';
        }
        unsigned long long v;
        switch (*fmt) {
        case 'd':
        case 'i': {
            long long s = lng >= 2 ? va_arg(args, long long) : va_arg(args, long);
            put_number(&k, s < 0 ? -(unsigned long long)s : (unsigned long long)s, s < 0, 10, 0, width, prec, left,
                       pad, sign);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o':
            v = lng >= 2 ? va_arg(args, unsigned long long) : va_arg(args, unsigned long);
            put_number(&k, v, 0, *fmt == 'o' ? 8 : *fmt == 'u' ? 10 : 16, *fmt == 'X', width, prec, left, pad, 0);
            break;
        case 'p':
            put(&k, '0');
            put(&k, 'x');
            put_number(&k, (uintptr_t)va_arg(args, void *), 0, 16, 0, 8, -1, 0, '0', 0);
            break;
        case 's': {
            const char *s = va_arg(args, const char *);
            if (s == 0) {
                s = "(null)";
            }
            int n = 0;
            while (s[n] && (prec < 0 || n < prec)) {
                n++;
            }
            for (int w = width; !left && w > n; w--) {
                put(&k, ' ');
            }
            for (int i = 0; i < n; i++) {
                put(&k, s[i]);
            }
            for (int w = width; left && w > n; w--) {
                put(&k, ' ');
            }
            break;
        }
        case 'c':
            put(&k, (char)va_arg(args, int));
            break;
        case '%':
            put(&k, '%');
            break;
        case 0:
            fmt--;
            break;
        default:
            put(&k, '%');
            put(&k, *fmt);
            break;
        }
    }
    if (size) {
        buf[k.len < size ? k.len : size - 1] = 0;
    }
    return (int)k.len;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, size, fmt, args);
    va_end(args);
    return n;
}

/* libphy formats into buffers it sized itself. */
int sprintf(char *buf, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, 0x7fffffff, fmt, args);
    va_end(args);
    return n;
}

/* --- The log's entry, ESP-IDF's lib_printf.c: libphy alone logs. --- */

int phy_printf(const char *fmt, ...);

#define LIB_PRINTF(name, tag)                                                                                          \
    int name(const char *fmt, ...)                                                                                     \
    {                                                                                                                  \
        va_list args;                                                                                                  \
        va_start(args, fmt);                                                                                           \
        drv_vsay(tag, fmt, args);                                                                                      \
        va_end(args);                                                                                                  \
        return 0;                                                                                                      \
    }
LIB_PRINTF(phy_printf, "phy")

/* The ESP32-C6's crystal is 40 MHz. */
int rtc_clk_xtal_freq_get(void);
int rtc_clk_xtal_freq_get(void)
{
    return 40;
}
