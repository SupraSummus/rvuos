#ifndef RVUOS_BOOTSEL_H
#define RVUOS_BOOTSEL_H

/*
 * Rebooting RP2350 into its BOOTSEL mode, through the watchdog.
 *
 * The watchdog's scratch registers tell the bootrom which boot the next one is:
 * a magic entry point and, in place of the stack pointer, a type, BOOTSEL's being 2;
 * the datasheet, "Watchdog boot vector".
 *
 * board.c arms it at boot with the longest count there is, about seventeen seconds, and nothing feeds it,
 * so every run on this board ends by then, and one that hangs comes back ready for the next image.
 * PSM_WDSEL has that reset everything but the oscillators, as the pico-sdk has it,
 * which keeps the scratch registers.
 * The halt arms it again for the host to read the log in, and reboots when it has as the bootrom's own reboot does:
 * everything but the processors' cold state, a millisecond after POWMAN's clock is moved off clk_ref, SYSCFG's AUXCTRL,
 * since resetting CLOCKS glitches clk_ref and may corrupt POWMAN, which only a power cycle resets.
 */

#include <stdint.h>

#define BOOTSEL_REG(addr) (*(volatile uint32_t *)(addr))

#define WATCHDOG_CTRL       0x400d8000u
#define WATCHDOG_LOAD       0x400d8004u
#define WATCHDOG_SCRATCH(n) (0x400d800cu + 4u * (n))
#define WATCHDOG_ENABLE     (1u << 30)
#define WATCHDOG_LONGEST    0x00ffffffu
#define BOOT_MAGIC          0xb007c0d3u
#define BOOT_TYPE_BOOTSEL   2u
#define PSM_WDSEL           0x40018008u
#define PSM_WDSEL_SET       (PSM_WDSEL + 0x2000u)
#define PSM_WDSEL_ALL       0x01ffffffu
#define PSM_WDSEL_PROC_COLD (1u << 0)
#define PSM_WDSEL_ROSC      (1u << 2)
#define PSM_WDSEL_XOSC      (1u << 3)
#define SYSCFG_AUXCTRL_SET  (0x40008014u + 0x2000u)
#define AUXCTRL_POWMAN_OFF_CLK_REF (1u << 0)

/* The watchdog stopped, and the next boot told to be BOOTSEL's. */
static inline void bootsel_scratch(void)
{
    BOOTSEL_REG(WATCHDOG_CTRL) = 0;
    BOOTSEL_REG(WATCHDOG_SCRATCH(2)) = 0; /* both of BOOTSEL's interfaces, no activity light */
    BOOTSEL_REG(WATCHDOG_SCRATCH(3)) = 0;
    BOOTSEL_REG(WATCHDOG_SCRATCH(4)) = BOOT_MAGIC;
    BOOTSEL_REG(WATCHDOG_SCRATCH(5)) = BOOT_MAGIC ^ -BOOT_MAGIC;
    BOOTSEL_REG(WATCHDOG_SCRATCH(6)) = BOOT_TYPE_BOOTSEL;
    BOOTSEL_REG(WATCHDOG_SCRATCH(7)) = BOOT_MAGIC;
}

/* Reboot into BOOTSEL once the watchdog has counted this many microseconds, at most WATCHDOG_LONGEST. */
static inline void bootsel_arm(uint32_t us)
{
    bootsel_scratch();
    BOOTSEL_REG(PSM_WDSEL_SET) = PSM_WDSEL_ALL & ~(PSM_WDSEL_ROSC | PSM_WDSEL_XOSC);
    BOOTSEL_REG(WATCHDOG_LOAD) = us;
    BOOTSEL_REG(WATCHDOG_CTRL) = WATCHDOG_ENABLE;
}

static inline __attribute__((noreturn)) void bootsel_now(void)
{
    bootsel_scratch();
    BOOTSEL_REG(PSM_WDSEL) = ~PSM_WDSEL_PROC_COLD;
    BOOTSEL_REG(WATCHDOG_LOAD) = 1000;
    BOOTSEL_REG(SYSCFG_AUXCTRL_SET) = AUXCTRL_POWMAN_OFF_CLK_REF;
    BOOTSEL_REG(WATCHDOG_CTRL) = WATCHDOG_ENABLE;
    for (;;) {
        __asm__ volatile("wfi");
    }
}

#endif
