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
 * the kernel writes the objects of a pool,
 * which on a device range would drive registers from machine mode.
 */
static inline bool ram_contains(uint32_t base, uint32_t size)
{
    return base >= RAM_BASE && base - RAM_BASE < RAM_SIZE && size <= RAM_SIZE - (base - RAM_BASE);
}

/*
 * The cores the kernel runs on, CORES of them, numbered from 0, the one that boots; see DESIGN.md, "Cores".
 * core_id says which one this is: the hart's mhartid on RISC-V, in the architecture's trap.c, or the host's.
 * With one core it is never asked.
 */
_Static_assert(CORES >= 1 && CORES <= 255, "a thread and a timer line name a core in a byte");
uint32_t core_id(void);

static inline uint32_t core_index(void)
{
    return CORES > 1 ? core_id() : 0;
}

/*
 * Not status codes: what a call tells syscall_dispatch instead of returning.
 * KERR_BLOCKED: the thread waits, and its registers are left alone until something wakes it.
 * KERR_PREEMPTED: the call stopped for a pending interrupt with its progress kept,
 * and starts again from its call instruction; see DESIGN.md, "Bounded work".
 */
#define KERR_BLOCKED   (-1)
#define KERR_PREEMPTED (-2)

/* main.c, entered from start.S. */
__attribute__((noreturn)) void kmain(void);

/* The board's board.c: what the board needs before anything else, watchdogs among it. */
void board_init(void);

/*
 * The board's, with more than one core: start the others at the end of the boot,
 * which go on in the architecture's core_start; see DESIGN.md, "Cores".
 */
void board_cores_start(void);

/*
 * The board's board.c: set back the CSRs user mode can write, so that nothing passes through them
 * from one process to the next; at boot, and whenever the processor goes to another process's thread.
 */
void board_user_csrs_reset(void);

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
