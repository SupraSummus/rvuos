#ifndef RVUOS_USER_RVUOS_H
#define RVUOS_USER_RVUOS_H

/*
 * User-side wrappers for capability invocation.
 * The ABI is in rvuos/abi.h.
 */

#include <stdint.h>

#include "rvuos/abi.h"

static inline uint32_t rv_invoke(uint32_t op, uint32_t cap,
                                 uint32_t a1, uint32_t a2, uint32_t a3)
{
    register uint32_t r_a0 __asm__("a0") = cap;
    register uint32_t r_a1 __asm__("a1") = a1;
    register uint32_t r_a2 __asm__("a2") = a2;
    register uint32_t r_a3 __asm__("a3") = a3;
    register uint32_t r_a7 __asm__("a7") = op;
    __asm__ volatile("ecall"
                     : "+r"(r_a0), "+r"(r_a1), "+r"(r_a2), "+r"(r_a3)
                     : "r"(r_a7)
                     : "memory", "a4", "a5", "a6");
    return r_a0;
}

/* OP_FRAME_INFO: where a frame points. */
static inline uint32_t rv_frame_info(uint32_t frame_cap, uint32_t *base, uint32_t *size)
{
    register uint32_t r_a0 __asm__("a0") = frame_cap;
    register uint32_t r_a1 __asm__("a1");
    register uint32_t r_a2 __asm__("a2");
    register uint32_t r_a7 __asm__("a7") = OP_FRAME_INFO;
    __asm__ volatile("ecall"
                     : "+r"(r_a0), "=r"(r_a1), "=r"(r_a2)
                     : "r"(r_a7)
                     : "memory", "a3", "a4", "a5", "a6");
    *base = r_a1;
    *size = r_a2;
    return r_a0;
}

/* OP_FRAME_INFO again, for the smallest region the machine protects. */
static inline uint32_t rv_frame_min_size(uint32_t frame_cap, uint32_t *min_out)
{
    register uint32_t r_a0 __asm__("a0") = frame_cap;
    register uint32_t r_a4 __asm__("a4");
    register uint32_t r_a7 __asm__("a7") = OP_FRAME_INFO;
    __asm__ volatile("ecall"
                     : "+r"(r_a0), "=r"(r_a4)
                     : "r"(r_a7)
                     : "memory", "a1", "a2", "a3", "a5", "a6");
    *min_out = r_a4;
    return r_a0;
}

/* OP_UNTYPED_INFO: where an Untyped's memory lies, and how much of it is made into something. */
static inline uint32_t rv_untyped_info(uint32_t untyped_cap, uint32_t *base, uint32_t *size,
                                       uint32_t *mark)
{
    register uint32_t r_a0 __asm__("a0") = untyped_cap;
    register uint32_t r_a1 __asm__("a1");
    register uint32_t r_a2 __asm__("a2");
    register uint32_t r_a4 __asm__("a4");
    register uint32_t r_a7 __asm__("a7") = OP_UNTYPED_INFO;
    __asm__ volatile("ecall"
                     : "+r"(r_a0), "=r"(r_a1), "=r"(r_a2), "=r"(r_a4)
                     : "r"(r_a7)
                     : "memory", "a3", "a5", "a6");
    *base = r_a1;
    *size = r_a2;
    *mark = r_a4;
    return r_a0;
}

/* OP_UNTYPED_RETYPE: make the next block of an Untyped into a frame, a pool or an Untyped. */
static inline uint32_t rv_retype(uint32_t untyped_cap, uint32_t type, uint32_t size, uint32_t dst,
                                 uint32_t *base)
{
    register uint32_t r_a0 __asm__("a0") = untyped_cap;
    register uint32_t r_a1 __asm__("a1") = type;
    register uint32_t r_a2 __asm__("a2") = size;
    register uint32_t r_a3 __asm__("a3") = dst;
    register uint32_t r_a7 __asm__("a7") = OP_UNTYPED_RETYPE;
    __asm__ volatile("ecall"
                     : "+r"(r_a0), "+r"(r_a1), "+r"(r_a2), "+r"(r_a3)
                     : "r"(r_a7)
                     : "memory", "a4", "a5", "a6");
    *base = r_a1;
    return r_a0;
}

/* OP_NOTIFY_SIGNAL: set bits, never blocking. */
static inline uint32_t rv_signal(uint32_t ntfn_cap, uint32_t bits)
{
    return rv_invoke(OP_NOTIFY_SIGNAL, ntfn_cap, bits, 0, 0);
}

/* OP_NOTIFY_WAIT: block until some bit is set, then take them all. */
static inline uint32_t rv_wait(uint32_t ntfn_cap, uint32_t *bits)
{
    register uint32_t r_a0 __asm__("a0") = ntfn_cap;
    register uint32_t r_a1 __asm__("a1");
    register uint32_t r_a7 __asm__("a7") = OP_NOTIFY_WAIT;
    __asm__ volatile("ecall"
                     : "+r"(r_a0), "=r"(r_a1)
                     : "r"(r_a7)
                     : "memory", "a2", "a3", "a4", "a5", "a6");
    *bits = r_a1;
    return r_a0;
}

/*
 * OP_IRQ_SET on a timer line: signal bits on the Irq's notification once us microseconds have passed,
 * no earlier and at the kernel's first tick after that. bits = 0 cancels.
 */
static inline uint32_t rv_timer_set(uint32_t timer_irq_cap, uint32_t bits, uint32_t us)
{
    return rv_invoke(OP_IRQ_SET, timer_irq_cap, bits, us, 0);
}

/*
 * OP_IRQ_SET: unmask the Irq's line and have the next interrupt signal bits.
 * The interrupt masks the line again, so this is also the acknowledgement. bits = 0 masks.
 */
static inline uint32_t rv_irq_set(uint32_t irq_cap, uint32_t bits)
{
    return rv_invoke(OP_IRQ_SET, irq_cap, bits, 0, 0);
}

/* OP_CLOCK_INFO: the counter's rate in Hz and the address of its low word. */
static inline uint32_t rv_clock_info(uint32_t clock_cap, uint32_t *hz, uint32_t *counter)
{
    register uint32_t r_a0 __asm__("a0") = clock_cap;
    register uint32_t r_a1 __asm__("a1");
    register uint32_t r_a2 __asm__("a2");
    register uint32_t r_a7 __asm__("a7") = OP_CLOCK_INFO;
    __asm__ volatile("ecall"
                     : "+r"(r_a0), "=r"(r_a1), "=r"(r_a2)
                     : "r"(r_a7)
                     : "memory", "a3", "a4", "a5", "a6");
    *hz = r_a1;
    *counter = r_a2;
    return r_a0;
}

/*
 * The counter, from the address OP_CLOCK_INFO gave, in a frame OP_CLOCK_FRAME gave.
 * No system call: the high word, the low word, and the high word again,
 * until the high word held still across the low one.
 */
static inline uint64_t rv_counter_read(uint32_t counter)
{
    const volatile uint32_t *word = (const volatile uint32_t *)counter;
    uint32_t hi, lo;
    do {
        hi = word[1];
        lo = word[0];
    } while (word[1] != hi);
    return ((uint64_t)hi << 32) | lo;
}

static inline void rv_putc(uint32_t debug_cap, char c)
{
    rv_invoke(OP_DEBUG_PUTC, debug_cap, (uint32_t)c, 0, 0);
}

static inline void rv_puts(uint32_t debug_cap, const char *s)
{
    while (*s != '\0') {
        rv_putc(debug_cap, *s++);
    }
}

static inline __attribute__((noreturn)) void rv_halt(uint32_t debug_cap, uint32_t code)
{
    rv_invoke(OP_DEBUG_HALT, debug_cap, code, 0, 0);
    for (;;) {
    }
}

#endif
