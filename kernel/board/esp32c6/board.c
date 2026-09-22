/*
 * What the ESP32-C6 needs before the kernel starts.
 *
 * The ROM hands over with three watchdogs running,
 * the main watchdogs of both timer groups and the RTC watchdog in the LP domain,
 * and the super watchdog behind them, any of which resets the chip within seconds.
 * The kernel has no use for them, so they go off first.
 *
 * The access permission management units, HP_APM, LP_APM0 and LP_APM,
 * filter every bus access by a security mode, and the CPU in user mode is not in theirs:
 * its peripheral reads return zero and its writes are dropped, with no fault to show for it.
 * PMP is what confines a process on rvuos, so the filters go off as well,
 * as ESP-IDF's startup turns them off; see DESIGN.md, "Boards".
 * Register addresses are those of Espressif's ESP-IDF, soc/esp32c6.
 */

#include <stdint.h>

#include "kernel.h"

#define REG(addr) (*(volatile uint32_t *)(addr))

/* Every watchdog's registers are write-protected until this key is written. */
#define WDT_KEY 0x50D83AA1u

#define TIMG_BASE(i)         (0x60008000u + 0x1000u * (i))
#define TIMG_WDTCONFIG0(i)   (TIMG_BASE(i) + 0x48u)
#define TIMG_WDTWPROTECT(i)  (TIMG_BASE(i) + 0x64u)
#define TIMG_WDT_CONF_UPDATE (1u << 22) /* the timer group takes the new configuration */

#define LP_WDT_BASE          0x600B1C00u
#define LP_WDT_CONFIG0       (LP_WDT_BASE + 0x00u)
#define LP_WDT_WPROTECT      (LP_WDT_BASE + 0x18u)
#define LP_WDT_SWD_CONFIG    (LP_WDT_BASE + 0x1cu)
#define LP_WDT_SWD_WPROTECT  (LP_WDT_BASE + 0x20u)
#define LP_WDT_SWD_DISABLE   (1u << 30)

/* One enable bit per access path; zero passes every access unfiltered. */
#define HP_APM_FUNC_CTRL     0x600990C4u
#define LP_APM0_FUNC_CTRL    0x600998C4u
#define LP_APM_FUNC_CTRL     0x600B38C4u

void board_init(void)
{
    REG(HP_APM_FUNC_CTRL) = 0;
    REG(LP_APM0_FUNC_CTRL) = 0;
    REG(LP_APM_FUNC_CTRL) = 0;

    for (unsigned i = 0; i < 2; i++) {
        REG(TIMG_WDTWPROTECT(i)) = WDT_KEY;
        REG(TIMG_WDTCONFIG0(i)) = 0;
        REG(TIMG_WDTCONFIG0(i)) = TIMG_WDT_CONF_UPDATE;
        REG(TIMG_WDTWPROTECT(i)) = 0;
    }

    REG(LP_WDT_WPROTECT) = WDT_KEY;
    REG(LP_WDT_CONFIG0) = 0;
    REG(LP_WDT_WPROTECT) = 0;

    REG(LP_WDT_SWD_WPROTECT) = WDT_KEY;
    REG(LP_WDT_SWD_CONFIG) |= LP_WDT_SWD_DISABLE;
    REG(LP_WDT_SWD_WPROTECT) = 0;
}
