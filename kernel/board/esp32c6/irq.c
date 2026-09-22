/*
 * Interrupt controller of the ESP32-C6: the interrupt matrix and the CPU's own controller.
 *
 * Every peripheral drives one source of the matrix, a level,
 * and the matrix routes each source to one of 31 CPU interrupts or to none.
 * The CPU's controller, PLIC_MX, gives each CPU interrupt an enable, a type and a priority,
 * and the core takes CPU interrupt n with mcause n,
 * as long as its bit in mie is set too.
 * rvuos routes every unmasked source to the one CPU interrupt IRQ_EXT_CAUSE
 * and masks a source by routing it nowhere,
 * so a line is a source and unmasking it is one write.
 * A claim finds a source whose level is high and which is routed there;
 * the matrix shows every source's level whether or not it is routed.
 * Nothing is held between a claim and a completion:
 * the kernel masks the line in between, which lowers the CPU interrupt on its own.
 * Register addresses are those of Espressif's ESP-IDF, soc/esp32c6.
 */

#include <stdint.h>

#include "csr.h"
#include "irq.h"

#define INTMTX_BASE      0x60010000u
#define INTMTX_MAP(src)  (INTMTX_BASE + 4u * (src))           /* the CPU interrupt, 0 for none */
#define INTMTX_STATUS(w) (INTMTX_BASE + 0x134u + 4u * (w))    /* one bit per source */

#define PLIC_MX_BASE    0x20001000u
#define PLIC_MX_ENABLE  (PLIC_MX_BASE + 0x00u)
#define PLIC_MX_TYPE    (PLIC_MX_BASE + 0x04u)                /* a set bit is edge, clear is level */
#define PLIC_MX_PRI(n)  (PLIC_MX_BASE + 0x10u + 4u * (n))
#define PLIC_MX_THRESH  (PLIC_MX_BASE + 0x90u)

#define REG(addr) (*(volatile uint32_t *)(addr))

#define CPU_INT     IRQ_EXT_CAUSE
#define CPU_INT_BIT (1u << CPU_INT)

/* The one CPU interrupt's priority, above a threshold that holds nothing back. */
#define CPU_INT_PRIORITY 1

void irq_enable(uint32_t line, bool on)
{
    REG(INTMTX_MAP(line)) = on ? CPU_INT : 0;
}

/*
 * Route every source nowhere, the ones the ROM routed too,
 * and have the core take only the one CPU interrupt, as a level.
 */
void irq_init(void)
{
    for (uint32_t line = 0; line < IRQ_LINES; line++) {
        REG(INTMTX_MAP(line)) = 0;
    }
    REG(PLIC_MX_TYPE) &= ~CPU_INT_BIT;
    REG(PLIC_MX_PRI(CPU_INT)) = CPU_INT_PRIORITY;
    REG(PLIC_MX_THRESH) = 0;
    REG(PLIC_MX_ENABLE) = CPU_INT_BIT;
    csr_set(mie, CPU_INT_BIT);
}

bool irq_enabled(uint32_t line)
{
    return REG(INTMTX_MAP(line)) == CPU_INT;
}

/* The lowest-numbered line that is high and unmasked; which of several comes first is not promised. */
uint32_t irq_claim(void)
{
    for (uint32_t word = 0; word < (IRQ_LINES + 31) / 32; word++) {
        uint32_t high = REG(INTMTX_STATUS(word));
        for (uint32_t bit = 0; high != 0 && bit < 32; bit++, high >>= 1) {
            uint32_t line = 32 * word + bit;
            if ((high & 1u) && line != 0 && line < IRQ_LINES && irq_enabled(line)) {
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
