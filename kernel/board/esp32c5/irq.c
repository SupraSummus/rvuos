/*
 * Interrupt controller of the ESP32-C5: the interrupt matrix and the CPU's CLIC.
 *
 * Every peripheral drives one source of the matrix, a level,
 * and the matrix routes each source to one of the CLIC's interrupts, the matrix's 32 numbered from 16.
 * The CLIC gives each interrupt an enable, a trigger and a level in a word of its own, see clic.h,
 * and the core takes interrupt n with mcause n.
 * rvuos routes every unmasked source to the one interrupt IRQ_EXT_CAUSE
 * and masks a source by routing it to interrupt 0, which the CLIC never enables, so unmasking a line is one write.
 * A line is a source plus one, so that line 0 stays the kernel's log
 * and source 0, the Wi-Fi MAC's, is line 1.
 * A claim finds a source whose level is high and which is routed there;
 * the matrix shows every source's level whether or not it is routed.
 * Nothing is held between a claim and a completion:
 * the kernel masks the line in between, which lowers the CLIC's interrupt on its own.
 * Register addresses are those of Espressif's ESP-IDF, soc/esp32c5.
 */

#include <stdint.h>

#include "clic.h"
#include "csr.h"
#include "irq.h"
#include "work.h"

#define INTMTX_BASE      0x60010000u
#define INTMTX_MAP(src)  (INTMTX_BASE + 4u * (src))           /* the CLIC's interrupt */
#define INTMTX_STATUS(w) (INTMTX_BASE + 0x150u + 4u * (w))    /* one bit per source */

/* The CLIC's interrupts the matrix raises, above the core's own 16, which are the arch's; see clic.h. */
#define CLIC_MATRIX_FIRST 16u
#define CLIC_MATRIX_END   48u

#define REG(addr) (*(volatile uint32_t *)(addr))

#define SOURCES     (IRQ_LINES - 1)
#define SOURCE(line) ((line) - 1u)

#define NOWHERE 0u

void irq_enable(uint32_t line, bool on)
{
    REG(INTMTX_MAP(SOURCE(line))) = on ? IRQ_EXT_CAUSE : NOWHERE;
}

/*
 * Route every source nowhere, the ones the ROM routed too,
 * take the enable away from every interrupt the matrix raises, the ROM's included,
 * and let the CLIC through at every level above its lowest, where the ROM leaves its threshold higher,
 * mintthresh reading its low four bits as ones;
 * then enable the one interrupt the matrix raises for rvuos.
 * The core's own interrupts are the arch's, the timer's enabled before this runs.
 */
void irq_init(void)
{
    for (uint32_t source = 0; source < SOURCES; source++) {
        REG(INTMTX_MAP(source)) = NOWHERE;
    }
    for (uint32_t n = CLIC_MATRIX_FIRST; n < CLIC_MATRIX_END; n++) {
        CLIC_WORD(n) &= ~CLIC_IE;
    }
    csr_write(0x347, 0); /* mintthresh */
    mie_set(1u << IRQ_EXT_CAUSE);
}

bool irq_enabled(uint32_t line)
{
    return REG(INTMTX_MAP(SOURCE(line))) == IRQ_EXT_CAUSE;
}

/* The lowest-numbered line that is high and unmasked; which of several comes first is not promised. */
uint32_t irq_claim(void)
{
    for (uint32_t word = 0; word < (SOURCES + 31) / 32; word++) {
        LOOP_BOUND((SOURCES + 31) / 32);
        uint32_t high = REG(INTMTX_STATUS(word));
        for (uint32_t bit = 0; high != 0 && bit < 32; bit++, high >>= 1) {
            LOOP_BOUND(32);
            uint32_t line = 32 * word + bit + 1;
            if ((high & 1u) && line < IRQ_LINES && irq_enabled(line)) {
                return line;
            }
        }
    }
    return 0;
}

void irq_complete(uint32_t line)
{
    (void)line;
}
