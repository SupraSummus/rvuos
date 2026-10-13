#ifndef RVUOS_USER_RVUOS_H
#define RVUOS_USER_RVUOS_H

/*
 * User-side wrappers for capability invocation.
 * The ABI is in rvuos/abi.h,
 * and the board's devices.h says how many device frames the boot grants, which BOOT_CAP_COUNT counts.
 */

#include <stdint.h>

#include "call.h"
#include "devices.h"
#include "rvuos/abi.h"

static inline uint32_t rv_invoke(uint32_t op, uint32_t cap,
                                 uint32_t a1, uint32_t a2, uint32_t a3)
{
    register uint32_t r_a0 __asm__(RV_A0) = cap;
    register uint32_t r_a1 __asm__(RV_A1) = a1;
    register uint32_t r_a2 __asm__(RV_A2) = a2;
    register uint32_t r_a3 __asm__(RV_A3) = a3;
    register uint32_t r_a7 __asm__(RV_A7) = op;
    __asm__ volatile(RV_CALL
                     : "+r"(r_a0), "+r"(r_a1), "+r"(r_a2), "+r"(r_a3)
                     : "r"(r_a7)
                     : "memory", RV_A4, RV_A5, RV_A6);
    return r_a0;
}

/* OP_CAP_COPY: a copy of the caller's src in slot dst of the table, with rights at most. */
static inline uint32_t rv_cap_copy(uint32_t table_cap, uint32_t dst, uint32_t src, uint32_t rights)
{
    return rv_invoke(OP_CAP_COPY, table_cap, dst, src, rights);
}

/* OP_CAP_DERIVE: as OP_CAP_COPY, but a child of src, which revoking below src takes. */
static inline uint32_t rv_cap_derive(uint32_t table_cap, uint32_t dst, uint32_t src, uint32_t rights)
{
    return rv_invoke(OP_CAP_DERIVE, table_cap, dst, src, rights);
}

/* OP_CAP_MOVE: the caller's src into slot dst of the table, in src's place in the derivation tree. */
static inline uint32_t rv_cap_move(uint32_t table_cap, uint32_t dst, uint32_t src)
{
    return rv_invoke(OP_CAP_MOVE, table_cap, dst, src, 0);
}

/* OP_CAP_DELETE: clear a slot of the table; what was derived from it goes to its parent. */
static inline uint32_t rv_cap_delete(uint32_t table_cap, uint32_t slot)
{
    return rv_invoke(OP_CAP_DELETE, table_cap, slot, 0, 0);
}

/* OP_CAP_REVOKE: clear everything derived from a slot of the table, everywhere, and leave the slot. */
static inline uint32_t rv_cap_revoke(uint32_t table_cap, uint32_t slot)
{
    return rv_invoke(OP_CAP_REVOKE, table_cap, slot, 0, 0);
}

/* OP_FRAME_CARVE: a smaller frame, a block of size at offset, in slot dst. */
static inline uint32_t rv_frame_carve(uint32_t frame_cap, uint32_t offset, uint32_t size, uint32_t dst)
{
    return rv_invoke(OP_FRAME_CARVE, frame_cap, offset, size, dst);
}

/* OP_POOL_ALLOC: an object of type in slot dst; arg is the type's, see rvuos/abi.h. */
static inline uint32_t rv_pool_alloc(uint32_t pool_cap, uint32_t type, uint32_t dst, uint32_t arg)
{
    return rv_invoke(OP_POOL_ALLOC, pool_cap, type, dst, arg);
}

/* OP_PROCESS_INSTALL: a frame in a region slot of the process, with rights at most the frame's. */
static inline uint32_t rv_process_install(uint32_t process_cap, uint32_t region, uint32_t frame_cap,
                                          uint32_t rights)
{
    return rv_invoke(OP_PROCESS_INSTALL, process_cap, region, frame_cap, rights);
}

/* OP_PROCESS_UNINSTALL: clear a region slot of the process. */
static inline uint32_t rv_process_uninstall(uint32_t process_cap, uint32_t region)
{
    return rv_invoke(OP_PROCESS_UNINSTALL, process_cap, region, 0, 0);
}

/* OP_PROCESS_MOUNT: a table in a table slot of the process, whose threads then name its slots SLOT_IN(index, i). */
static inline uint32_t rv_process_mount(uint32_t process_cap, uint32_t index, uint32_t table_cap)
{
    return rv_invoke(OP_PROCESS_MOUNT, process_cap, index, table_cap, 0);
}

/* OP_PROCESS_UNMOUNT: clear a table slot of the process. */
static inline uint32_t rv_process_unmount(uint32_t process_cap, uint32_t index)
{
    return rv_invoke(OP_PROCESS_UNMOUNT, process_cap, index, 0, 0);
}

/* OP_THREAD_CONFIGURE: where a stopped thread starts, and the argument its entry is called with. */
static inline uint32_t rv_thread_configure(uint32_t thread_cap, uint32_t pc, uint32_t sp, uint32_t arg)
{
    return rv_invoke(OP_THREAD_CONFIGURE, thread_cap, pc, sp, arg);
}

/* OP_THREAD_RESUME: make a stopped thread ready. */
static inline uint32_t rv_thread_resume(uint32_t thread_cap)
{
    return rv_invoke(OP_THREAD_RESUME, thread_cap, 0, 0, 0);
}

/*
 * OP_THREAD_STOP: stop a thread, whatever it is doing; *was = what it was doing, THREAD_WAS_*.
 * a1 to a3 are zero, as the call asks.
 */
static inline uint32_t rv_thread_stop(uint32_t thread_cap, uint32_t *was)
{
    register uint32_t r_a0 __asm__(RV_A0) = thread_cap;
    register uint32_t r_a1 __asm__(RV_A1) = 0;
    register uint32_t r_a2 __asm__(RV_A2) = 0;
    register uint32_t r_a3 __asm__(RV_A3) = 0;
    register uint32_t r_a7 __asm__(RV_A7) = OP_THREAD_STOP;
    __asm__ volatile(RV_CALL
                     : "+r"(r_a0), "+r"(r_a1)
                     : "r"(r_a2), "r"(r_a3), "r"(r_a7)
                     : "memory", RV_A4, RV_A5, RV_A6);
    *was = r_a1;
    return r_a0;
}

/* OP_THREAD_WATCH: the bits the thread's faults signal on a notification; bits = 0 clears the watch. */
static inline uint32_t rv_thread_watch(uint32_t thread_cap, uint32_t ntfn_cap, uint32_t bits)
{
    return rv_invoke(OP_THREAD_WATCH, thread_cap, ntfn_cap, bits, 0);
}

/* OP_THREAD_WRITE_REG: one register of a stopped thread, or its program counter, THREAD_REG_PC. */
static inline uint32_t rv_thread_write_reg(uint32_t thread_cap, uint32_t reg, uint32_t value)
{
    return rv_invoke(OP_THREAD_WRITE_REG, thread_cap, reg, value, 0);
}

/* OP_IRQ_CARVE: a smaller range of lines, count from offset, in slot dst. */
static inline uint32_t rv_irq_carve(uint32_t lines_cap, uint32_t offset, uint32_t count, uint32_t dst)
{
    return rv_invoke(OP_IRQ_CARVE, lines_cap, offset, count, dst);
}

/* OP_IRQ_BIND: the one line of line_cap bound to a notification, as an Irq from the pool in slot dst. */
static inline uint32_t rv_irq_bind(uint32_t line_cap, uint32_t pool_cap, uint32_t ntfn_cap, uint32_t dst)
{
    return rv_invoke(OP_IRQ_BIND, line_cap, pool_cap, ntfn_cap, dst);
}

/* OP_CLOCK_FRAME: a read-only frame over the counter, in slot dst. */
static inline uint32_t rv_clock_frame(uint32_t clock_cap, uint32_t dst)
{
    return rv_invoke(OP_CLOCK_FRAME, clock_cap, dst, 0, 0);
}

/* OP_TIME_CARVE: a smaller range of units, count from offset, in slot dst. */
static inline uint32_t rv_time_carve(uint32_t time_cap, uint32_t offset, uint32_t count, uint32_t dst)
{
    return rv_invoke(OP_TIME_CARVE, time_cap, offset, count, dst);
}

/* OP_TIME_BIND: a thread bound to count units of the capability from offset, which it then earns. */
static inline uint32_t rv_time_bind(uint32_t time_cap, uint32_t thread_cap, uint32_t offset, uint32_t count)
{
    return rv_invoke(OP_TIME_BIND, time_cap, thread_cap, offset, count);
}

/* OP_FRAME_INFO: where a frame points. */
static inline uint32_t rv_frame_info(uint32_t frame_cap, uint32_t *base, uint32_t *size)
{
    register uint32_t r_a0 __asm__(RV_A0) = frame_cap;
    register uint32_t r_a1 __asm__(RV_A1) = 0;
    register uint32_t r_a2 __asm__(RV_A2) = 0;
    register uint32_t r_a3 __asm__(RV_A3) = 0;
    register uint32_t r_a7 __asm__(RV_A7) = OP_FRAME_INFO;
    __asm__ volatile(RV_CALL
                     : "+r"(r_a0), "+r"(r_a1), "+r"(r_a2), "+r"(r_a3)
                     : "r"(r_a7)
                     : "memory", RV_A4, RV_A5, RV_A6);
    *base = r_a1;
    *size = r_a2;
    return r_a0;
}

/* OP_FRAME_INFO again, for the smallest region the machine protects. */
static inline uint32_t rv_frame_min_size(uint32_t frame_cap, uint32_t *min_out)
{
    register uint32_t r_a0 __asm__(RV_A0) = frame_cap;
    register uint32_t r_a1 __asm__(RV_A1) = 0;
    register uint32_t r_a2 __asm__(RV_A2) = 0;
    register uint32_t r_a3 __asm__(RV_A3) = 0;
    register uint32_t r_a4 __asm__(RV_A4);
    register uint32_t r_a7 __asm__(RV_A7) = OP_FRAME_INFO;
    __asm__ volatile(RV_CALL
                     : "+r"(r_a0), "+r"(r_a1), "+r"(r_a2), "+r"(r_a3), "=r"(r_a4)
                     : "r"(r_a7)
                     : "memory", RV_A5, RV_A6);
    *min_out = r_a4;
    return r_a0;
}

/* OP_UNTYPED_INFO: where an Untyped's memory lies, and whether something made of it is left. */
static inline uint32_t rv_untyped_info(uint32_t untyped_cap, uint32_t *base, uint32_t *size,
                                       uint32_t *made)
{
    register uint32_t r_a0 __asm__(RV_A0) = untyped_cap;
    register uint32_t r_a1 __asm__(RV_A1) = 0;
    register uint32_t r_a2 __asm__(RV_A2) = 0;
    register uint32_t r_a3 __asm__(RV_A3) = 0;
    register uint32_t r_a4 __asm__(RV_A4);
    register uint32_t r_a7 __asm__(RV_A7) = OP_UNTYPED_INFO;
    __asm__ volatile(RV_CALL
                     : "+r"(r_a0), "+r"(r_a1), "+r"(r_a2), "+r"(r_a3), "=r"(r_a4)
                     : "r"(r_a7)
                     : "memory", RV_A5, RV_A6);
    *base = r_a1;
    *size = r_a2;
    *made = r_a4;
    return r_a0;
}

/*
 * OP_CAP_INFO: what a capability is, *type one of CAP_* and *rights its rights,
 * and in w the three words its type tells, see rvuos/abi.h. Page 0, the only one.
 */
static inline uint32_t rv_cap_info(uint32_t cap, uint32_t *type, uint32_t *rights, uint32_t w[3])
{
    register uint32_t r_a0 __asm__(RV_A0) = cap;
    register uint32_t r_a1 __asm__(RV_A1) = 0;
    register uint32_t r_a2 __asm__(RV_A2) = 0;
    register uint32_t r_a3 __asm__(RV_A3) = 0;
    register uint32_t r_a4 __asm__(RV_A4);
    register uint32_t r_a7 __asm__(RV_A7) = OP_CAP_INFO;
    __asm__ volatile(RV_CALL
                     : "+r"(r_a0), "+r"(r_a1), "+r"(r_a2), "+r"(r_a3), "=r"(r_a4)
                     : "r"(r_a7)
                     : "memory", RV_A5, RV_A6);
    *type = CAP_INFO_TYPE(r_a1);
    *rights = CAP_INFO_RIGHTS(r_a1);
    w[0] = r_a2;
    w[1] = r_a3;
    w[2] = r_a4;
    return r_a0;
}

/* OP_POOL_INFO: the bytes an object of a type takes of a pool, slots its count for a table, and the bytes left. */
static inline uint32_t rv_pool_info(uint32_t pool_cap, uint32_t type, uint32_t slots, uint32_t *bytes, uint32_t *left)
{
    register uint32_t r_a0 __asm__(RV_A0) = pool_cap;
    register uint32_t r_a1 __asm__(RV_A1) = type;
    register uint32_t r_a2 __asm__(RV_A2) = 0;
    register uint32_t r_a3 __asm__(RV_A3) = slots;
    register uint32_t r_a7 __asm__(RV_A7) = OP_POOL_INFO;
    __asm__ volatile(RV_CALL
                     : "+r"(r_a0), "+r"(r_a1), "+r"(r_a2)
                     : "r"(r_a3), "r"(r_a7)
                     : "memory", RV_A4, RV_A5, RV_A6);
    *bytes = r_a1;
    *left = r_a2;
    return r_a0;
}

/* OP_UNTYPED_RETYPE: make the whole of an Untyped into a frame or a pool. */
static inline uint32_t rv_retype(uint32_t untyped_cap, uint32_t type, uint32_t dst, uint32_t *base)
{
    register uint32_t r_a0 __asm__(RV_A0) = untyped_cap;
    register uint32_t r_a1 __asm__(RV_A1) = type;
    register uint32_t r_a2 __asm__(RV_A2) = dst;
    register uint32_t r_a3 __asm__(RV_A3) = 0;
    register uint32_t r_a7 __asm__(RV_A7) = OP_UNTYPED_RETYPE;
    __asm__ volatile(RV_CALL
                     : "+r"(r_a0), "+r"(r_a1), "+r"(r_a2), "+r"(r_a3)
                     : "r"(r_a7)
                     : "memory", RV_A4, RV_A5, RV_A6);
    *base = r_a1;
    return r_a0;
}

/* OP_UNTYPED_SPLIT: split an Untyped into its lower and upper halves. */
static inline uint32_t rv_split(uint32_t untyped_cap, uint32_t lower, uint32_t upper)
{
    return rv_invoke(OP_UNTYPED_SPLIT, untyped_cap, lower, upper, 0);
}

/* OP_THREAD_FAULT: the cause, program counter, address and status of the fault a thread is stopped at. */
static inline uint32_t rv_thread_fault(uint32_t thread_cap, uint32_t *cause, uint32_t *pc, uint32_t *addr,
                                       uint32_t *status)
{
    register uint32_t r_a0 __asm__(RV_A0) = thread_cap;
    register uint32_t r_a1 __asm__(RV_A1) = 0;
    register uint32_t r_a2 __asm__(RV_A2) = 0;
    register uint32_t r_a3 __asm__(RV_A3) = 0;
    register uint32_t r_a4 __asm__(RV_A4);
    register uint32_t r_a7 __asm__(RV_A7) = OP_THREAD_FAULT;
    __asm__ volatile(RV_CALL
                     : "+r"(r_a0), "+r"(r_a1), "+r"(r_a2), "+r"(r_a3), "=r"(r_a4)
                     : "r"(r_a7)
                     : "memory", RV_A5, RV_A6);
    *cause = r_a1;
    *pc = r_a2;
    *addr = r_a3;
    *status = r_a4;
    return r_a0;
}

/* OP_THREAD_READ_REG: one register of a stopped thread, or its program counter, THREAD_REG_PC. */
static inline uint32_t rv_thread_read_reg(uint32_t thread_cap, uint32_t reg, uint32_t *value)
{
    register uint32_t r_a0 __asm__(RV_A0) = thread_cap;
    register uint32_t r_a1 __asm__(RV_A1) = reg;
    register uint32_t r_a2 __asm__(RV_A2) = 0;
    register uint32_t r_a3 __asm__(RV_A3) = 0;
    register uint32_t r_a7 __asm__(RV_A7) = OP_THREAD_READ_REG;
    __asm__ volatile(RV_CALL
                     : "+r"(r_a0), "+r"(r_a1), "+r"(r_a2), "+r"(r_a3)
                     : "r"(r_a7)
                     : "memory", RV_A4, RV_A5, RV_A6);
    *value = r_a1;
    return r_a0;
}

/* OP_NOTIFY_SIGNAL: set bits, never blocking. */
static inline uint32_t rv_signal(uint32_t ntfn_cap, uint32_t bits)
{
    return rv_invoke(OP_NOTIFY_SIGNAL, ntfn_cap, bits, 0, 0);
}

/* OP_NOTIFY_CARVE: a capability to the same notification that may signal only those of bits the invoked one may. */
static inline uint32_t rv_notify_carve(uint32_t ntfn_cap, uint32_t bits, uint32_t dst)
{
    return rv_invoke(OP_NOTIFY_CARVE, ntfn_cap, bits, dst, 0);
}

/* OP_NOTIFY_WAIT: block until some bit is set, then take them all. */
static inline uint32_t rv_wait(uint32_t ntfn_cap, uint32_t *bits)
{
    register uint32_t r_a0 __asm__(RV_A0) = ntfn_cap;
    register uint32_t r_a1 __asm__(RV_A1) = 0;
    register uint32_t r_a2 __asm__(RV_A2) = 0;
    register uint32_t r_a3 __asm__(RV_A3) = 0;
    register uint32_t r_a7 __asm__(RV_A7) = OP_NOTIFY_WAIT;
    __asm__ volatile(RV_CALL
                     : "+r"(r_a0), "+r"(r_a1), "+r"(r_a2), "+r"(r_a3)
                     : "r"(r_a7)
                     : "memory", RV_A4, RV_A5, RV_A6);
    *bits = r_a1;
    return r_a0;
}

/* OP_NOTIFY_LEND: as rv_wait, lending the thread of thread_cap, which has RIGHT_X, the caller's time meanwhile. */
static inline uint32_t rv_lend(uint32_t ntfn_cap, uint32_t thread_cap, uint32_t *bits)
{
    register uint32_t r_a0 __asm__(RV_A0) = ntfn_cap;
    register uint32_t r_a1 __asm__(RV_A1) = thread_cap;
    register uint32_t r_a2 __asm__(RV_A2) = 0;
    register uint32_t r_a3 __asm__(RV_A3) = 0;
    register uint32_t r_a7 __asm__(RV_A7) = OP_NOTIFY_LEND;
    __asm__ volatile(RV_CALL
                     : "+r"(r_a0), "+r"(r_a1), "+r"(r_a2), "+r"(r_a3)
                     : "r"(r_a7)
                     : "memory", RV_A4, RV_A5, RV_A6);
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
 * OP_IRQ_SET on a timer line with IRQ_SET_PERIOD: signal bits a whole number of periods
 * after the line's last deadline, at the first such tick still ahead.
 * *skipped receives the periods that had passed by the call.
 */
static inline uint32_t rv_timer_period(uint32_t timer_irq_cap, uint32_t bits, uint32_t us,
                                       uint32_t *skipped)
{
    register uint32_t r_a0 __asm__(RV_A0) = timer_irq_cap;
    register uint32_t r_a1 __asm__(RV_A1) = bits;
    register uint32_t r_a2 __asm__(RV_A2) = us;
    register uint32_t r_a3 __asm__(RV_A3) = IRQ_SET_PERIOD;
    register uint32_t r_a7 __asm__(RV_A7) = OP_IRQ_SET;
    __asm__ volatile(RV_CALL
                     : "+r"(r_a0), "+r"(r_a1)
                     : "r"(r_a2), "r"(r_a3), "r"(r_a7)
                     : "memory", RV_A4, RV_A5, RV_A6);
    *skipped = r_a1;
    return r_a0;
}

/*
 * OP_IRQ_SET: unmask the Irq's line and have the next interrupt signal bits.
 * The interrupt masks the line again, so this is also the acknowledgement. bits = 0 masks.
 */
static inline uint32_t rv_irq_set(uint32_t irq_cap, uint32_t bits)
{
    return rv_invoke(OP_IRQ_SET, irq_cap, bits, 0, 0);
}

/* OP_CLOCK_READ: the time in counts since boot, the counter's rate in Hz, and the address of its low word. */
static inline uint32_t rv_clock_read(uint32_t clock_cap, uint64_t *now, uint32_t *hz, uint32_t *counter)
{
    register uint32_t r_a0 __asm__(RV_A0) = clock_cap;
    register uint32_t r_a1 __asm__(RV_A1) = 0;
    register uint32_t r_a2 __asm__(RV_A2) = 0;
    register uint32_t r_a3 __asm__(RV_A3) = 0;
    register uint32_t r_a4 __asm__(RV_A4);
    register uint32_t r_a7 __asm__(RV_A7) = OP_CLOCK_READ;
    __asm__ volatile(RV_CALL
                     : "+r"(r_a0), "+r"(r_a1), "+r"(r_a2), "+r"(r_a3), "=r"(r_a4)
                     : "r"(r_a7)
                     : "memory", RV_A5, RV_A6);
    *now = ((uint64_t)r_a2 << 32) | r_a1;
    *hz = r_a3;
    *counter = r_a4;
    return r_a0;
}

/* OP_CLOCK_WATCHDOG: halt the machine unless this is called again within us microseconds. */
static inline uint32_t rv_clock_watchdog(uint32_t clock_cap, uint32_t us)
{
    return rv_invoke(OP_CLOCK_WATCHDOG, clock_cap, us, 0, 0);
}

/*
 * The counter's low 32 bits, from the address OP_CLOCK_READ gave, in a frame OP_CLOCK_FRAME gave:
 * a load and no system call. They wrap, so a program takes differences of them.
 */
static inline uint32_t rv_counter_low(uint32_t counter)
{
    return *(const volatile uint32_t *)counter;
}

/* OP_PRINT: the first n bytes of s, PRINT_BYTES at most, into the kernel's log; a zero byte ends them. */
static inline uint32_t rv_print(uint32_t print_cap, const char *s, uint32_t n)
{
    uint32_t w[3] = { 0, 0, 0 };
    for (uint32_t i = 0; i < n && i < PRINT_BYTES; i++) {
        w[i / 4] |= (uint32_t)(uint8_t)s[i] << (8 * (i % 4));
    }
    return rv_invoke(OP_PRINT, print_cap, w[0], w[1], w[2]);
}

/* n bytes of text into the kernel's log, PRINT_BYTES a call. */
static inline void rv_write(uint32_t print_cap, const char *s, uint32_t n)
{
    for (uint32_t at = 0; at < n; at += PRINT_BYTES) {
        rv_print(print_cap, s + at, n - at);
    }
}

static inline void rv_putc(uint32_t print_cap, char c)
{
    rv_print(print_cap, &c, 1);
}

static inline void rv_puts(uint32_t print_cap, const char *s)
{
    uint32_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    rv_write(print_cap, s, n);
}

static inline void rv_put_hex(uint32_t print_cap, uint32_t v)
{
    char text[10] = { '0', 'x' };
    for (int i = 0; i < 8; i++) {
        text[2 + i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 0xfu];
    }
    rv_write(print_cap, text, sizeof(text));
}

static inline __attribute__((noreturn)) void rv_halt(uint32_t halt_cap, uint32_t code)
{
    rv_invoke(OP_HALT, halt_cap, code, 0, 0);
    for (;;) {
    }
}

/* OP_TEST_TRACE, OP_TEST_TICK, OP_TEST_IRQ and OP_TEST_PREEMPT, for tests; see rvuos/abi.h. */
static inline uint32_t rv_test_trace(uint32_t test_cap)
{
    return rv_invoke(OP_TEST_TRACE, test_cap, 0, 0, 0);
}

static inline uint32_t rv_test_tick(uint32_t test_cap)
{
    return rv_invoke(OP_TEST_TICK, test_cap, 0, 0, 0);
}

static inline uint32_t rv_test_irq(uint32_t test_cap, uint32_t line)
{
    return rv_invoke(OP_TEST_IRQ, test_cap, line, 0, 0);
}

static inline uint32_t rv_test_preempt(uint32_t test_cap, uint32_t n)
{
    return rv_invoke(OP_TEST_PREEMPT, test_cap, n, 0, 0);
}

#endif
