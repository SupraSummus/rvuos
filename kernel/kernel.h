#ifndef RVUOS_KERNEL_H
#define RVUOS_KERNEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rvuos/abi.h"
#include "layout.h"
#include "paddr.h"
#include "work.h"

/*
 * True if [base, base + size) lies within RAM.
 * Kernel objects live in RAM alone:
 * a pool is zeroed and written by the kernel,
 * which on a device range would drive registers from machine mode.
 */
static inline bool ram_contains(uint32_t base, uint32_t size)
{
    return base >= RAM_BASE && base - RAM_BASE < RAM_SIZE && size <= RAM_SIZE - (base - RAM_BASE);
}

/*
 * Not status codes: what a call tells syscall_dispatch instead of returning.
 * KERR_BLOCKED: the thread waits, and its registers are left alone until something wakes it.
 * KERR_PREEMPTED: the call stopped for a pending interrupt with its progress kept,
 * and starts again from its ecall; see DESIGN.md, "Bounded work".
 */
#define KERR_BLOCKED   (-1)
#define KERR_PREEMPTED (-2)

/* main.c, entered from start.S. */
__attribute__((noreturn)) void kmain(void);

/* The board's board.c: what the board needs before anything else, watchdogs among it. */
void board_init(void);

/*
 * Minimal output into the kernel's log; formatted printing is not worth a printf yet.
 * kputc, in panic.c, appends to the log, and klog.c builds the other two on it.
 */
void kputc(char c);
void kputs(const char *s);
void kput_hex(uint32_t v);
/* A literal's length bounds each call; the parentheses call the function around the macro. */
#define kputs(s)             \
    do {                     \
        CALL_LITERAL(s);     \
        (kputs)(s);          \
    } while (0)

/* string.c; the compiler may also emit calls to these. */
void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);

/* Stop the machine. Under QEMU this exits the emulator with the code; see the board's halt.c. */
__attribute__((noreturn)) void khalt(int code);

__attribute__((noreturn)) void kpanic(const char *msg);

/* An invariant failed; the report has been printed. Stop in a way tests notice. */
__attribute__((noreturn)) void selfcheck_fail(void);

#endif
