#ifndef RVUOS_LIB_SAY_H
#define RVUOS_LIB_SAY_H

/*
 * Text with values, as printf puts it, to wherever a process's text goes:
 * a console, a child's log, a buffer.
 */

#include <stdint.h>

/* Where text goes: a function and what it is handed. */
struct out {
    void (*put)(void *to, char c);
    void *to;
};

/*
 * The conversions a system needs:
 * %s a string, %u and %d decimal, %x hexadecimal with 0x,
 * %M the six bytes of a hardware address, %I an IPv4 address in network order.
 */
void say(const struct out *o, const char *fmt, ...);

#endif
