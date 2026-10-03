#include "lib/say.h"

#include <stdarg.h>

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
