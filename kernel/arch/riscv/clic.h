#ifndef RVUOS_CLIC_H
#define RVUOS_CLIC_H

/*
 * The enables and pending bits of the interrupts the kernel takes, mie's and mip's,
 * on a core with the standard controller and on one with a CLIC.
 *
 * A board whose core has a CLIC, as the ESP32-C5 has, names its interrupts' control words in board.h,
 * CLIC_CTRL, one word per interrupt from there, and then the core runs in CLIC mode alone:
 * mtvec's mode reads 3 whatever is written, every trap enters at its base,
 * mie is ignored and mip reads zero,
 * and an interrupt is taken only while its own enable is set in its word and its level is above mintthresh.
 * mcause keeps the previous mode, interrupt enable and level above its code,
 * which mret restores, so the kernel reads the code as its low twelve bits and the interrupt bit.
 * mie_set, mie_clear and mip_read stand for csr_set(mie, bits), csr_clear(mie, bits) and csr_read(mip) on either core,
 * with the CLIC's bits in mip's places for the timer, the software and the board's external interrupt,
 * each of which the CLIC numbers as mcause does.
 */

#include <stdint.h>

#include "csr.h"
#include "layout.h"

#ifdef CLIC_CTRL

#define CLIC_WORD(n)  (*(volatile uint32_t *)(CLIC_CTRL + 4u * (n)))
#define CLIC_IP       (1u << 0)
#define CLIC_IE       (1u << 8)
#define CLIC_MODE_M   (3u << 22) /* taken in machine mode, a level, not vectored */
#define CLIC_LEVEL_TOP (0xffu << 24)

/* The low twelve bits of mcause are its code under the CLIC, and the top its interrupt bit. */
#define MCAUSE_CAUSE (MCAUSE_INTERRUPT | 0xfffu)

/*
 * Each interrupt named in bits is enabled, at the top level, in machine mode, as a level:
 * the kernel takes only the timer's, the software's and IRQ_EXT_CAUSE.
 */
static inline void clic_enable(uint32_t n)
{
    CLIC_WORD(n) = CLIC_LEVEL_TOP | CLIC_MODE_M | CLIC_IE;
}

_Static_assert(IRQ_EXT_CAUSE < 32, "the external interrupt has a place in mip");

static inline void mie_set(uint32_t bits)
{
    if (bits & MIE_MSIE) {
        clic_enable(IRQ_M_SOFT);
    }
    if (bits & MIE_MTIE) {
        clic_enable(IRQ_M_TIMER);
    }
    if (bits & (1u << IRQ_EXT_CAUSE)) {
        clic_enable(IRQ_EXT_CAUSE);
    }
}

static inline void mie_clear(uint32_t bits)
{
    if (bits & MIE_MSIE) {
        CLIC_WORD(IRQ_M_SOFT) &= ~CLIC_IE;
    }
    if (bits & MIE_MTIE) {
        CLIC_WORD(IRQ_M_TIMER) &= ~CLIC_IE;
    }
    if (bits & (1u << IRQ_EXT_CAUSE)) {
        CLIC_WORD(IRQ_EXT_CAUSE) &= ~CLIC_IE;
    }
}

static inline uint32_t mip_read(void)
{
    uint32_t ip = (CLIC_WORD(IRQ_M_TIMER) & CLIC_IP) << IRQ_M_TIMER;
    ip |= (CLIC_WORD(IRQ_EXT_CAUSE) & CLIC_IP) << IRQ_EXT_CAUSE;
#if CORES > 1
    ip |= (CLIC_WORD(IRQ_M_SOFT) & CLIC_IP) << IRQ_M_SOFT;
#endif
    return ip;
}

#else

#define MCAUSE_CAUSE 0xffffffffu

static inline void mie_set(uint32_t bits)
{
    csr_set(mie, bits);
}

static inline void mie_clear(uint32_t bits)
{
    csr_clear(mie, bits);
}

static inline uint32_t mip_read(void)
{
    return csr_read(mip);
}

#endif

#endif
