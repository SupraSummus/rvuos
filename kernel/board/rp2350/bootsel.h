#ifndef RVUOS_BOOTSEL_H
#define RVUOS_BOOTSEL_H

/*
 * Rebooting RP2350 into its BOOTSEL mode, through the watchdog.
 *
 * The watchdog's scratch registers tell the bootrom which boot the next one is:
 * a magic entry point and, in place of the stack pointer, a type, BOOTSEL's being 2;
 * the datasheet, "Watchdog boot vector".
 * PSM_WDSEL has the watchdog reset everything but the oscillators, as the pico-sdk has it,
 * which keeps the scratch registers.
 *
 * board.c arms it at boot with the longest count there is, about seventeen seconds, and nothing feeds it,
 * so every run on this board ends by then, and one that hangs comes back ready for the next image.
 * The halt arms it again for the host to read the log in, and reboots at once when it has.
 */

#include <stdint.h>

#define BOOTSEL_REG(addr) (*(volatile uint32_t *)(addr))

#define WATCHDOG_CTRL       0x400d8000u
#define WATCHDOG_LOAD       0x400d8004u
#define WATCHDOG_SCRATCH(n) (0x400d800cu + 4u * (n))
#define WATCHDOG_ENABLE     (1u << 30)
#define WATCHDOG_TRIGGER    (1u << 31)
#define WATCHDOG_LONGEST    0x00ffffffu
#define BOOT_MAGIC          0xb007c0d3u
#define BOOT_TYPE_BOOTSEL   2u
#define PSM_WDSEL_SET       (0x40018008u + 0x2000u)
#define PSM_WDSEL_ALL       0x01ffffffu
#define PSM_WDSEL_ROSC      (1u << 2)
#define PSM_WDSEL_XOSC      (1u << 3)

/* Reboot into BOOTSEL once the watchdog has counted this many microseconds, at most WATCHDOG_LONGEST. */
static inline void bootsel_arm(uint32_t us)
{
    BOOTSEL_REG(WATCHDOG_CTRL) = 0;
    BOOTSEL_REG(WATCHDOG_SCRATCH(2)) = 0; /* both of BOOTSEL's interfaces, no activity light */
    BOOTSEL_REG(WATCHDOG_SCRATCH(3)) = 0;
    BOOTSEL_REG(WATCHDOG_SCRATCH(4)) = BOOT_MAGIC;
    BOOTSEL_REG(WATCHDOG_SCRATCH(5)) = BOOT_MAGIC ^ -BOOT_MAGIC;
    BOOTSEL_REG(WATCHDOG_SCRATCH(6)) = BOOT_TYPE_BOOTSEL;
    BOOTSEL_REG(WATCHDOG_SCRATCH(7)) = BOOT_MAGIC;
    BOOTSEL_REG(PSM_WDSEL_SET) = PSM_WDSEL_ALL & ~(PSM_WDSEL_ROSC | PSM_WDSEL_XOSC);
    BOOTSEL_REG(WATCHDOG_LOAD) = us;
    BOOTSEL_REG(WATCHDOG_CTRL) = WATCHDOG_ENABLE;
}

static inline __attribute__((noreturn)) void bootsel_now(void)
{
    bootsel_arm(WATCHDOG_LONGEST);
    BOOTSEL_REG(WATCHDOG_CTRL) = WATCHDOG_ENABLE | WATCHDOG_TRIGGER;
    for (;;) {
        __asm__ volatile("wfi");
    }
}

#endif
