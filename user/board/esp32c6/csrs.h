#ifndef RVUOS_USER_CSRS_H
#define RVUOS_USER_CSRS_H

/*
 * The CSRs user mode writes on the ESP32-C6; see kernel/board/esp32c6/board.c.
 * Whenever another process has run, a process finds the performance counter as it left it and the rest set back;
 * a new process finds them all set back.
 * The demo's root task starts the counter before it takes turns with its child, and finds it counting on after;
 * it marks them all before it hands over, and its successor finds them set back.
 * Each board's csrs.h offers the same four functions.
 */

#include <stdbool.h>
#include <stdint.h>

/* Each by number, the root task's mark, and what the kernel sets it back to. */
#define CSRS(X)                                          \
    X(0x000, 0x11, 0)       /* ustatus: UIE and UPIE */  \
    X(0x004, 0x111, 0)      /* uie */                    \
    X(0x005, 0x40820001, 1) /* utvec */                  \
    X(0x041, 0x40820000, 0) /* uepc */                   \
    X(0x042, 0x2, 0)        /* ucause */                 \
    X(0x800, 0x1, 0)        /* mpcer: count cycles */    \
    X(0x801, 0x1, 0)        /* mpcmr: count */           \
    X(0x803, 0x5a, 0)       /* cpu_gpio_oen */           \
    X(0x805, 0xa5, 0)       /* cpu_gpio_out */

#define csr_read(csr) ({ uint32_t v_; __asm__ volatile("csrr %0, " #csr : "=r"(v_)); v_; })
#define CSR_MARK(csr, mark, reset)      __asm__ volatile("csrw " #csr ", %0" : : "r"((uint32_t)(mark)));
#define CSR_IS_MARKED(csr, mark, reset) && csr_read(csr) == (mark)
#define CSR_IS_RESET(csr, mark, reset)  && csr_read(csr) == (reset)

/* Whether every mark took; the count, 0x802, runs from the zero it was set back to. */
static inline bool csrs_mark(void)
{
    CSRS(CSR_MARK)
    return csr_read(0x802) != 0 CSRS(CSR_IS_MARKED);
}

static inline bool csrs_are_reset(void)
{
    return csr_read(0x802) == 0 CSRS(CSR_IS_RESET);
}

/* Starts the counter on cycles, and says where it starts from. */
static inline uint32_t counter_start(void)
{
    __asm__ volatile("csrw 0x800, %0\n\tcsrw 0x801, %0" : : "r"(1));
    return csr_read(0x802);
}

/* Whether the counter still counts cycles, on past from. */
static inline bool counter_kept(uint32_t from)
{
    return csr_read(0x800) == 1 && csr_read(0x801) == 1 && (int32_t)(csr_read(0x802) - from) > 0;
}

#endif
