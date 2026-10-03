#ifndef RVUOS_LIB_SAY_H
#define RVUOS_LIB_SAY_H

/*
 * Text with values, as printf puts it, to wherever a process's text goes:
 * the kernel's log, a console, a buffer.
 */

#include <stdint.h>

/* Where text goes: a function handed it a piece at a time, and what it is handed. */
struct out {
    void (*write)(void *to, const char *s, uint32_t n);
    void *to;
};

/*
 * The conversions a system needs:
 * %s a string, %u and %d decimal, %x hexadecimal with 0x,
 * %M the six bytes of a hardware address, %I an IPv4 address in network order.
 * The text goes out in pieces of SAY_PIECE bytes, the last when the call ends,
 * so a line of the kernel's log costs a call per DEBUG_WRITE_BYTES of it and no more.
 */
void say(const struct out *o, const char *fmt, ...);
#define SAY_PIECE 48u

#endif
