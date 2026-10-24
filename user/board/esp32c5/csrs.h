#ifndef RVUOS_USER_CSRS_H
#define RVUOS_USER_CSRS_H

/*
 * The CSRs user mode writes on the ESP32-C5; see kernel/board/esp32c5/board.c.
 * Whenever another process has run, a process finds them set back, and so does a new one:
 * the core has no performance counter user mode reaches, so nothing is a process's to keep.
 * The demo's root task marks them all before it hands over, and its successor finds them set back.
 * Under the CLIC, ucause holds ustatus's UPIE in its bit 27, as mcause holds mstatus's MPIE,
 * so the mark of one is the mark of the other.
 * Each board's csrs.h offers the same four functions.
 */

#include <stdbool.h>
#include <stdint.h>

/* Each by number, the root task's mark, and what the kernel sets it back to, as each reads. */
#define CSRS(X)                                              \
    X(0x000, 0x11, 0)       /* ustatus: UIE and UPIE */      \
    X(0x005, 0x40820003, 3) /* utvec, whose mode reads 3 */ \
    X(0x007, 0x40820000, 0) /* utvt */                       \
    X(0x017, 0x40820040, 0) /* undocumented, a base */       \
    X(0x040, 0x5a5a5a5a, 0) /* uscratch */                   \
    X(0x041, 0x40820000, 0) /* uepc */                       \
    X(0x042, 0x08000002, 0) /* ucause, with ustatus's UPIE */ \
    X(0x047, 0xff, 0xf)     /* uintthresh */                 \
    X(0x803, 0x5a, 0)       /* cpu_gpio_oen */               \
    X(0x805, 0xa5, 0)       /* cpu_gpio_out */

#define csr_read(csr) ({ uint32_t v_; __asm__ volatile("csrr %0, " #csr : "=r"(v_)); v_; })
#define CSR_MARK(csr, mark, reset)      __asm__ volatile("csrw " #csr ", %0" : : "r"((uint32_t)(mark)));
#define CSR_IS_MARKED(csr, mark, reset) && csr_read(csr) == (mark)
#define CSR_IS_RESET(csr, mark, reset)  && csr_read(csr) == (reset)

/* Whether every mark took. */
static inline bool csrs_mark(void)
{
    CSRS(CSR_MARK)
    return true CSRS(CSR_IS_MARKED);
}

static inline bool csrs_are_reset(void)
{
    return true CSRS(CSR_IS_RESET);
}

/* Nothing to start, and nothing to keep. */
static inline uint32_t counter_start(void)
{
    return 0;
}

static inline bool counter_kept(uint32_t from)
{
    (void)from;
    return true;
}

#endif
