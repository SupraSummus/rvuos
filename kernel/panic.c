/*
 * Output and the two ways the kernel gives up, the same on every board.
 * How the machine then stops is the board's khalt, in its halt.c;
 * the host build has its own versions of all four, in host/shim.c.
 */

#include "kernel.h"
#include "klog.h"

/* The kernel has no console: every byte goes into its log; see DESIGN.md, "The kernel log". */
void kputc(char c)
{
    klog_append(c);
}

void selfcheck_fail(void)
{
    khalt(3);
}

void kpanic(const char *msg)
{
    kputs("kernel panic: ");
    (kputs)(msg); /* on the way to the halt, so its length bounds nothing */
    kputc('\n');
    khalt(1);
}
