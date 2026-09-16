#ifndef RVUOS_CSR_H
#define RVUOS_CSR_H

#include <stdint.h>

/*
 * CSR access.
 * The CSR name must be a compile-time token,
 * so these are macros rather than functions.
 */

#define csr_read(csr)                                          \
    ({                                                         \
        uint32_t v_;                                           \
        __asm__ volatile("csrr %0, " #csr : "=r"(v_));         \
        v_;                                                    \
    })

#define csr_write(csr, val)                                    \
    do {                                                       \
        uint32_t v_ = (val);                                   \
        __asm__ volatile("csrw " #csr ", %0" : : "r"(v_));     \
    } while (0)

#define csr_set(csr, mask)                                     \
    do {                                                       \
        uint32_t v_ = (mask);                                  \
        __asm__ volatile("csrs " #csr ", %0" : : "r"(v_));     \
    } while (0)

#define csr_clear(csr, mask)                                   \
    do {                                                       \
        uint32_t v_ = (mask);                                  \
        __asm__ volatile("csrc " #csr ", %0" : : "r"(v_));     \
    } while (0)

/* mstatus fields. */
#define MSTATUS_MIE  (1u << 3)
#define MSTATUS_MPIE (1u << 7)
#define MSTATUS_MPP_SHIFT 11
#define MSTATUS_MPP_MASK  (3u << MSTATUS_MPP_SHIFT)
#define MSTATUS_MPP_U (0u << MSTATUS_MPP_SHIFT)
#define MSTATUS_MPP_M (3u << MSTATUS_MPP_SHIFT)

/* mcause exception codes; interrupts have the top bit set. */
#define MCAUSE_INTERRUPT (1u << 31)
#define CAUSE_INSN_MISALIGNED   0
#define CAUSE_INSN_ACCESS       1
#define CAUSE_ILLEGAL_INSN      2
#define CAUSE_BREAKPOINT        3
#define CAUSE_LOAD_MISALIGNED   4
#define CAUSE_LOAD_ACCESS       5
#define CAUSE_STORE_MISALIGNED  6
#define CAUSE_STORE_ACCESS      7
#define CAUSE_ECALL_U           8
#define CAUSE_ECALL_S           9
#define CAUSE_ECALL_M           11

#endif
