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
 * With them goes what they could do for the modem's DMA, which open decision 13 leaves to trust.
 *
 * The ROM hands over the SAR ADC, a device board.h lists, with its registers held in reset, where writes are dropped,
 * and its and the temperature sensor's clocks off, as measured on the chip.
 * No program reaches PCR, so the kernel turns the clocks on and lets the registers out of reset,
 * and does as much for SPI2, another device board.h lists, which it resets first,
 * since a reset of the chip keeps PCR's and the controller's state alike;
 * its function clock is PLL_F80M, 80 MHz, the most the controller takes, which the program divides further in SPI2's own registers.
 *
 * Espressif's PHY library, libphy.a in the Wi-Fi driver, also sets a few registers of the PMU and LP_AON,
 * which no program reaches, and only one way:
 * the analog I2C buses its calibration talks over powered, the baseband's I2C bus tied powered,
 * and the power detector's capacitance.
 * The kernel sets them at boot, and the driver's own versions of those functions leave them out;
 * the ROM leaves only the peripheral I2C bus powered, as measured on the chip.
 * The crystal's tick count the PHY writes into PCR is already 40 MHz's, as the chip resets.
 * The CPU's clock, in PCR too, is timer.c's, which sets it before it measures the counter that counts it.
 *
 * The pins stay as the ROM leaves them: how a board wires them, as the XIAO ESP32-C6 does its RF switch,
 * is for a program to set, through the IO MUX and the GPIO matrix board.h lists.
 *
 * The MMU's only valid entries are the image's 64 KiB pages and the window's, see board.h, each run in order,
 * which the kernel sets, with the cache emptied of what the pages held before, then lets the cache's buses through.
 *
 * The core has user-mode traps, the N extension,
 * and resets with mideleg at 0x111, delegating the user software, timer and external interrupts,
 * 0, 4 and 8, to a handler in user mode that no process set up and the kernel does not switch.
 * Every trap is the kernel's, so nothing is delegated.
 * Register addresses are those of Espressif's ESP-IDF, soc/esp32c6.
 */

#include <stdint.h>

#include "csr.h"
#include "kernel.h"
#include "object.h"

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

/* The SAR ADC's clocks and resets, and the temperature sensor's clock, in PCR. */
#define PCR_SARADC_CONF       0x60096080u
#define PCR_SARADC_CLK_EN     (1u << 0)
#define PCR_SARADC_REG_CLK_EN (1u << 2) /* its registers' clock; bit 3 holds them in reset */
#define PCR_TSENS_CLK_CONF    0x60096088u
#define PCR_TSENS_CLK_EN      (1u << 22)
/* SPI2's clocks and reset, in PCR. */
#define PCR_SPI2_CONF         0x600960C0u
#define PCR_SPI2_CLK_EN       (1u << 0) /* its registers' clock */
#define PCR_SPI2_RST_EN       (1u << 1)
#define PCR_SPI2_CLKM_CONF    0x600960C4u
#define PCR_SPI2_CLKM_80M     ((1u << 22) | (1u << 20)) /* CLKM_EN, PLL_F80M */

/* The radio's analog power, in the PMU, and the power detector's capacitance, in LP_AON. */
#define PMU_IMM_HP_CK_POWER      0x600B00CCu
#define PMU_TIE_HIGH_XPD_BB_I2C  (1u << 28) /* written, never read: ties the baseband's I2C bus powered */
#define PMU_RF_PWC               0x600B0154u
#define PMU_RF_I2C_ON            0xFC000000u /* PERIF_I2C_RSTB and the XPD of PERIF_I2C to PLL_I2C */
#define LP_AON_SAR_CCT           0x600B1054u
#define LP_AON_SAR2_PWDET_CCT    (7u << 29)
#define LP_AON_SAR2_PWDET_CCT_4  (4u << 29)

/* The MMU, which maps 64 KiB pages of flash through the cache from 0x42000000, one entry each, and the cache. */
#define MMU_ITEM_CONTENT     0x6000237Cu
#define MMU_ITEM_INDEX       0x60002380u
#define MMU_BASE             0x42000000u
#define MMU_ENTRIES          256u
#define MMU_PAGE             0x10000u
#define MMU_VALID            (1u << 9)
#define EXTMEM_L1_CACHE_CTRL (0x600C8000u + 0x04u)
#define EXTMEM_SHUT_BUSES    0x3u /* IBUS and DBUS */
#define EXTMEM_SYNC_CTRL     (0x600C8000u + 0x98u)
#define EXTMEM_SYNC_ADDR     (0x600C8000u + 0xa0u)
#define EXTMEM_SYNC_SIZE     (0x600C8000u + 0xa4u)
#define EXTMEM_INVALIDATE    (1u << 0)
#define EXTMEM_SYNC_DONE     (1u << 4)

_Static_assert(FLASH_WINDOW_BASE % MMU_PAGE == 0 && FLASH_WINDOW_FLASH % MMU_PAGE == 0 &&
                   FLASH_WINDOW_SIZE % MMU_PAGE == 0,
               "the window is whole pages");
_Static_assert(FLASH_IMAGE_BASE % MMU_PAGE == 0 && FLASH_IMAGE_SIZE % MMU_PAGE == 0 &&
                   USER_CODE_FLASH + USER_CODE_SIZE <= INPUT_FLASH && INPUT_FLASH + INPUT_SIZE <= FLASH_IMAGE_SIZE,
               "the image is whole pages, the root task's code and input in it");
_Static_assert(FLASH_WINDOW_BASE >= MMU_BASE && FLASH_IMAGE_BASE >= FLASH_WINDOW_BASE + FLASH_WINDOW_SIZE &&
                   FLASH_IMAGE_BASE + FLASH_IMAGE_SIZE <= MMU_BASE + MMU_PAGE * MMU_ENTRIES &&
                   FLASH_IMAGE_SIZE <= FLASH_WINDOW_FLASH,
               "the window and the image lie apart in the MMU's space and in flash");

/* The flash page entry i of the MMU shows, or 0 for none. */
static uint32_t mmu_item(uint32_t i)
{
    uint32_t at = MMU_BASE + i * MMU_PAGE;
    if (at - FLASH_WINDOW_BASE < FLASH_WINDOW_SIZE) {
        return MMU_VALID | (FLASH_WINDOW_FLASH + (at - FLASH_WINDOW_BASE)) / MMU_PAGE;
    }
    if (at - FLASH_IMAGE_BASE < FLASH_IMAGE_SIZE) {
        return MMU_VALID | (at - FLASH_IMAGE_BASE) / MMU_PAGE;
    }
    return 0;
}

static void flash_map(void)
{
    for (uint32_t i = 0; i < MMU_ENTRIES; i++) {
        REG(MMU_ITEM_INDEX) = i;
        REG(MMU_ITEM_CONTENT) = mmu_item(i);
    }
    /* A size of zero empties the whole cache. */
    REG(EXTMEM_SYNC_ADDR) = 0;
    REG(EXTMEM_SYNC_SIZE) &= 0xff000000u;
    REG(EXTMEM_SYNC_CTRL) |= EXTMEM_INVALIDATE;
    while (!(REG(EXTMEM_SYNC_CTRL) & EXTMEM_SYNC_DONE)) {
    }
    REG(EXTMEM_L1_CACHE_CTRL) &= ~EXTMEM_SHUT_BUSES;
}

void board_init(void)
{
    csr_write(mideleg, 0);

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

    REG(PCR_SARADC_CONF) = PCR_SARADC_CLK_EN | PCR_SARADC_REG_CLK_EN;
    REG(PCR_TSENS_CLK_CONF) |= PCR_TSENS_CLK_EN;
    REG(PCR_SPI2_CONF) = PCR_SPI2_CLK_EN | PCR_SPI2_RST_EN;
    REG(PCR_SPI2_CONF) = PCR_SPI2_CLK_EN;
    REG(PCR_SPI2_CLKM_CONF) = PCR_SPI2_CLKM_80M;

    REG(PMU_RF_PWC) |= PMU_RF_I2C_ON;
    REG(PMU_IMM_HP_CK_POWER) = PMU_TIE_HIGH_XPD_BB_I2C;
    REG(LP_AON_SAR_CCT) = (REG(LP_AON_SAR_CCT) & ~LP_AON_SAR2_PWDET_CCT) | LP_AON_SAR2_PWDET_CCT_4;

    flash_map();
}

/*
 * The CSRs user mode writes, measured on the chip; see DESIGN.md, "Boards".
 * By number, since the assembler names none of them.
 * The performance counter is the process's, see BOARD_PROCESS_CSRS: it stops while the process is away,
 * and counts on from where it was when the process is back, as the ROM's delays need;
 * it is left stopped at zero, counting nothing, for a process that never set it and for none.
 * The rest is set back.
 */
void board_user_csrs_switch(struct process *from, struct process *to)
{
    static const uint32_t none[BOARD_PROCESS_CSRS];
    csr_clear(mstatus, 0x11); /* UIE and UPIE, which user mode writes as ustatus */
    csr_write(0x004, 0);      /* uie */
    csr_write(0x005, 1);      /* utvec, whose mode bit stays set */
    csr_write(0x041, 0);      /* uepc */
    csr_write(0x042, 0);      /* ucause */
    csr_write(0x803, 0);      /* cpu_gpio_oen */
    csr_write(0x805, 0);      /* cpu_gpio_out */
    if (from != NULL) {
        from->csrs[0] = csr_read(0x7e0); /* mpcer: what the counter counts */
        from->csrs[1] = csr_read(0x7e1); /* mpcmr: whether it counts */
        from->csrs[2] = csr_read(0x7e2); /* mpccr: the count */
    }
    /* Stopped while it is set, so that it starts from the count kept. */
    const uint32_t *kept = to != NULL ? to->csrs : none;
    csr_write(0x7e1, 0);
    csr_write(0x7e0, kept[0]);
    csr_write(0x7e2, kept[2]);
    csr_write(0x7e1, kept[1]);
}

/* The chip's watchdogs stay off, as board_init leaves them, so a kernel that stopped altogether stays stopped. */
void board_watchdog(uint32_t us)
{
    (void)us;
}
