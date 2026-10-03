#include "lib/say.h"

#include <stdarg.h>

#include "rvuos/abi.h"

/* What say has formatted and not yet handed on: whole writes into the kernel's log, so a line costs no more calls. */
struct pending {
    const struct out *o;
    uint32_t n;
    char buf[4 * DEBUG_WRITE_BYTES];
};

static void flush(struct pending *p)
{
    if (p->n != 0) {
        p->o->write(p->o->to, p->buf, p->n);
        p->n = 0;
    }
}

static void put(struct pending *p, char c)
{
    p->buf[p->n++] = c;
    if (p->n == sizeof(p->buf)) {
        flush(p);
    }
}

static void put_hex(struct pending *o, uint32_t v, int digits)
{
    for (int shift = 4 * (digits - 1); shift >= 0; shift -= 4) {
        put(o, "0123456789abcdef"[(v >> shift) & 0xfu]);
    }
}

static void put_dec(struct pending *o, uint32_t v)
{
    char buf[10];
    int i = 0;
    do {
        buf[i++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (i) {
        put(o, buf[--i]);
    }
}

void say(const struct out *out, const char *fmt, ...)
{
    struct pending pending;
    struct pending *o = &pending;
    o->o = out;
    o->n = 0;
    va_list ap;
    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%' || fmt[1] == '\0') {
            put(o, *fmt);
            continue;
        }
        switch (*++fmt) {
        case 's':
            for (const char *s = va_arg(ap, const char *); *s; s++) {
                put(o, *s);
            }
            break;
        case 'u':
            put_dec(o, va_arg(ap, uint32_t));
            break;
        case 'd': {
            int32_t v = va_arg(ap, int32_t);
            if (v < 0) {
                put(o, '-');
            }
            put_dec(o, v < 0 ? 0u - (uint32_t)v : (uint32_t)v);
            break;
        }
        case 'x':
            put(o, '0');
            put(o, 'x');
            put_hex(o, va_arg(ap, uint32_t), 8);
            break;
        case 'M': {
            const uint8_t *mac = va_arg(ap, const uint8_t *);
            for (int i = 0; i < 6; i++) {
                put_hex(o, mac[i], 2);
                if (i < 5) {
                    put(o, ':');
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
                    put(o, '.');
                }
            }
            break;
        }
        default:
            put(o, *fmt);
        }
    }
    va_end(ap);
    flush(o);
}
