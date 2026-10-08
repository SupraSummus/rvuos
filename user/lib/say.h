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
 * %M the six bytes of a hardware address, %I an IPv4 address in network order, %% a percent sign.
 * A conversion it does not know, a width among them, is named in the text, as [say: unknown %0] for %08x,
 * and the text past it goes out as it stands, no argument read, since whether it took one cannot be told.
 * The text goes out in a few pieces, the last as the call ends.
 */
void say(const struct out *o, const char *fmt, ...);

#endif
