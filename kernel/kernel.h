#ifndef RVUOS_KERNEL_H
#define RVUOS_KERNEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rvuos/abi.h"
#include "paddr.h"

/*
 * Memory layout of the QEMU virt target, shared with kernel.ld and user.ld.
 * The kernel owns [RAM_BASE, USER_CODE_BASE), with its log at the top of that,
 * touching the root task's code so that the two cost one PMP boundary.
 * The root task's code and data regions follow,
 * and everything after them is handed to the root task as free RAM.
 */
#define RAM_BASE       0x80000000u
#define RAM_SIZE       0x00800000u
#define USER_CODE_BASE 0x80100000u
#define USER_CODE_SIZE 0x00010000u
#define USER_DATA_BASE 0x80200000u
#define USER_DATA_SIZE 0x00010000u
/* The last 64 KiB of RAM hold test input placed there by the loader. */
#define INPUT_SIZE     0x00010000u
#define INPUT_BASE     (RAM_BASE + RAM_SIZE - INPUT_SIZE)
/* The kernel's log, see klog.h: its header and KLOG_SIZE bytes of ring. */
#define KLOG_SIZE      0x00001000u
#define KLOG_BASE      (USER_CODE_BASE - RVUOS_LOG_HEADER - KLOG_SIZE)

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

/* main.c, entered from start.S. */
__attribute__((noreturn)) void kmain(void);

/*
 * Minimal output into the kernel's log; formatted printing is not worth a printf yet.
 * kputc is the board's, halt.c on QEMU, and klog.c builds the other two on it.
 */
void kputc(char c);
void kputs(const char *s);
void kput_hex(uint32_t v);

/* string.c; the compiler may also emit calls to these. */
void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);

/* Stop the machine. Under QEMU this exits the emulator with the code. */
__attribute__((noreturn)) void khalt(int code);

__attribute__((noreturn)) void kpanic(const char *msg);

/* An invariant failed; the report has been printed. Stop in a way tests notice. */
__attribute__((noreturn)) void selfcheck_fail(void);

#endif
