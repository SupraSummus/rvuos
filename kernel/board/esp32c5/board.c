/*
 * What the ESP32-C5 needs before the kernel starts.
 *
 * The ROM hands over with watchdogs running, as on the ESP32-C6:
 * the main watchdogs of both timer groups, the RTC watchdog in the LP domain and the super watchdog behind them.
 * The kernel has no use for them, so they go off first.
 *
 * Two filters stand between the CPU in user mode and the bus, both of which ESP-IDF, running all in machine mode, never meets.
 * The access permission management units, HP_APM, LP_APM0, LP_APM and CPU_APM, filter accesses by a security mode,
 * and with them on user mode cannot even fetch from SRAM.
 * The TEE controllers, HP's and LP's, give each peripheral a read and a write permission for each security mode,
 * and reset to letting only the TEE mode, machine mode's, through:
 * user mode then reads every peripheral as zero, and its writes are dropped, with no fault to show for it.
 * PMP is what confines a process on rvuos, so the APMs go off and every TEE permission is granted to every mode,
 * as measured on the chip; see DESIGN.md, "Boards".
 * With them goes what they could do for the modem's DMA, which open decision 13 leaves to trust.
 * Nothing opens the CPU's own devices, the CLINT and the CLIC, to user mode, whose loads there fault.
 *
 * The pins stay as the ROM leaves them: how a board wires them is for a program to set,
 * through the IO MUX and the GPIO matrix board.h lists.
 *
 * No program reaches PCR, which holds the whole chip's clocks and resets,
 * so the kernel turns on the clocks of the one controller board.h lists, SPI2, and lets it out of reset.
 * It resets SPI2 first, since a reset of the chip keeps PCR's and the controller's state alike,
 * so a program finds the controller as it comes out of reset, whatever the last one left.
 * Its function clock is PLL_F160M halved, 80 MHz, the most the controller takes; the program divides it further in SPI2's own registers.
 *
 * The ROM hands over the SAR ADC, which board.h lists, with its and the temperature sensor's clocks off,
 * so the kernel turns them on and lets the ADC's registers out of reset, as Espressif's PHY library does.
 *
 * That library, libphy.a, which the Wi-Fi driver runs while the radio's own code is not yet its own,
 * also sets a few registers of the PMU, PCR and LP_AON, which no program reaches, and only one way:
 * the analog I2C buses its calibration talks over powered, the 5 GHz ones among them, the baseband's I2C bus tied powered,
 * a word of PCR's that ESP-IDF leaves reserved, beside the SAR ADC's clock dividers, whose low byte it sets to 10,
 * and the power detector's capacitance.
 * The kernel sets them at boot, as it does on the ESP32-C6, and the driver feigns the library's accesses to them.
 *
 * The random number generator, which board.h lists read only, comes from the ROM stirring in two timers
 * but not sampling its ring oscillator, which the kernel turns on, as ESP-IDF does at its start.
 *
 * The system bus, its arbiter and router, resets at power-on alone while PCR's RESET_EVENT_BYPASS is set, as it is at power-on,
 * so a reset in the middle of a cache's read of flash, a watchdog's or the USB Serial/JTAG's, leaves it stopped:
 * every boot after hangs in the ROM's first read of flash, until the chip's enable pin or its power resets it.
 * The kernel clears the bit first thing, as ESP-IDF's bootloader does, so the next reset takes the bus with it,
 * whenever it comes; measured on the chip, resets in the middle of the cache's misses boot cleanly with it clear, and hang with it set.
 *
 * The MMU's only valid entries are the image's 64 KiB pages and the window's, see board.h, each run in order, which the kernel sets,
 * and in a kernel built with PSRAM=1 the PSRAM's, which mspi.c brings up later,
 * with the cache emptied of what the pages held before, then lets the cache's buses through.
 *
 * The core has user-mode traps, the N extension, and the CLIC's user-mode CSRs,
 * and resets with mideleg at zero; the kernel clears it anyway, since every trap is its own.
 * Register addresses are those of Espressif's ESP-IDF, soc/esp32c5.
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
#define CPU_APM_FUNC_CTRL    0x6009A0C4u
#define LP_APM_FUNC_CTRL     0x600B38C4u

/*
 * Each TEE controller's permission words, one a peripheral from the first to the last,
 * with a read bit for each of the four security modes in the low nibble and a write bit for each in the next.
 * The words between them that ESP-IDF names nothing in are written too.
 * The HP controller's run past the last its register header names, the bit scrambler's at 0x140,
 * to the modem's power's at 0x148, which soc/apm_defs.h names: the MAC's power and timer block, MODEM_LPCON
 * and the analog I2C master, without which a radio's program reads them as zero and its writes there are lost.
 */
#define TEE_PERI_FIRST       0x60098088u /* UART0's */
#define TEE_PERI_LAST        0x60098158u /* the SLC host's, the last of ESP-IDF's soc/apm_defs.h */
#define LP_TEE_PERI_FIRST    0x600B3404u /* the eFuse's */
#define LP_TEE_PERI_LAST     0x600B344Cu /* LP_APM's */
#define TEE_EVERY_MODE       0xFFu

/* Which resets reach the system bus: with this bit set, a power-on's alone. */
#define PCR_RESET_EVENT_BYPASS 0x60096174u
#define PCR_BYPASS_SYSTEM_BUS  (1u << 1)
/* SPI2's clocks and reset, in PCR. */
#define PCR_SPI2_CONF        0x600960C4u
#define PCR_SPI2_CLK_EN      (1u << 0) /* its registers' clock */
#define PCR_SPI2_RST_EN      (1u << 1)
#define PCR_SPI2_CLKM_CONF   0x600960C8u
#define PCR_SPI2_CLKM_80M    ((1u << 22) | (1u << 20) | (1u << 12)) /* CLKM_EN, PLL_F160M, divided by DIV_NUM 1 plus one */

/* The SAR ADC's clocks and reset, and the temperature sensor's clock, in PCR. */
#define PCR_SARADC_CONF       0x60096088u
#define PCR_SARADC_CLK_EN     (1u << 0)
#define PCR_SARADC_REG_CLK_EN (1u << 2) /* its registers' clock; bit 3 holds them in reset */
#define PCR_TSENS_CLK_CONF    0x60096090u
#define PCR_TSENS_CLK_XTAL    (1u << 20) /* TSENS_CLK_SEL */
#define PCR_TSENS_CLK_EN      (1u << 22)
#define PCR_TSENS_RST_EN      (1u << 23)
#define PCR_RESERVED_14C      0x6009614Cu
#define PCR_RESERVED_14C_LOW  0xFFu
#define PCR_RESERVED_14C_PHY  10u

/* The radio's analog power, in the PMU, and the power detector's capacitance, in LP_AON. */
#define PMU_IMM_HP_CK_POWER     0x600B00CCu
#define PMU_TIE_HIGH_XPD_BB_I2C (1u << 28) /* written, never read: ties the baseband's I2C bus powered */
#define PMU_RF_PWC              0x600B0158u
#define PMU_RF_I2C_ON           0xFB800000u /* XPD_CKGEN5G, TC5G_I2C, RX5G_I2C, XPD_PERIF_I2C to XPD_PLL_I2C */
#define PMU_PERIF_I2C_RSTB      (1u << 26)
#define LP_AON_SAR_CCT          0x600B1054u
#define LP_AON_SAR2_PWDET_CCT   (7u << 29)
#define LP_AON_SAR2_PWDET_CCT_4 (4u << 29)

/*
 * The modem's clock gating code while the PMU is active, which its clock domains' maps are read with:
 * a domain whose map has the code's bit set keeps its clock. It powers on as 0, which no map a radio's program sets has,
 * so the front end's power detector reads zero and the PHY's calibration never ends. ESP-IDF's bootloader sets 2,
 * rtc_clk_modem_clock_domain_active_state_icg_map_preinit, and the code takes effect when the PMU is told to update it.
 * The PMU keeps it through a reset, so a chip an ESP-IDF image ran on since its power came on calibrates without it.
 */
#define PMU_HP_ACTIVE_ICG_MODEM   0x600B000Cu
#define PMU_ICG_MODEM_CODE_ACTIVE (2u << 30)
#define PMU_IMM_SLEEP_SYSCLK      0x600B00D0u
#define PMU_UPDATE_DIG_ICG_SWITCH (1u << 28)
#define PMU_IMM_MODEM_ICG         0x600B00DCu
#define PMU_UPDATE_ICG_MODEM      (1u << 31)

/* LPPERI's RNG_CFG, whose low bit has the generator sample its ring oscillator. */
#define LPPERI_RNG_CFG       0x600B2824u
#define RNG_SAMPLE_ENABLE    (1u << 0)

/* The MMU, which maps 64 KiB pages of flash through the cache from 0x42000000, one entry each, and the cache. */
#define MMU_ITEM_CONTENT     0x6000237Cu
#define MMU_ITEM_INDEX       0x60002380u
#define MMU_BASE             0x42000000u
#define MMU_ENTRIES          512u
#define MMU_PAGE             0x10000u
#define MMU_VALID            (1u << 10)
#define MMU_PSRAM            (1u << 9) /* the page is the PSRAM's, not the flash's */
#define CACHE_L1_CACHE_CTRL  (0x600C8000u + 0x04u)
#define CACHE_SHUT_BUSES     0x3u /* IBUS and DBUS */
#define CACHE_SYNC_CTRL      (0x600C8000u + 0x94u)
#define CACHE_SYNC_ADDR      (0x600C8000u + 0x9cu)
#define CACHE_SYNC_SIZE      (0x600C8000u + 0xa0u)
#define CACHE_INVALIDATE     (1u << 0)
#define CACHE_SYNC_DONE      (1u << 4)

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
#ifdef PSRAM
_Static_assert(PSRAM_BASE % MMU_PAGE == 0 && PSRAM_SIZE % MMU_PAGE == 0 && PSRAM_BASE >= FLASH_IMAGE_BASE + FLASH_IMAGE_SIZE &&
                   PSRAM_BASE + PSRAM_SIZE <= MMU_BASE + MMU_PAGE * MMU_ENTRIES,
               "the PSRAM is whole pages, past the image in the MMU's space");
#endif

/* The page of flash or PSRAM entry i of the MMU shows, or 0 for none. */
static uint32_t mmu_item(uint32_t i)
{
    uint32_t at = MMU_BASE + i * MMU_PAGE;
    if (at - FLASH_WINDOW_BASE < FLASH_WINDOW_SIZE) {
        return MMU_VALID | (FLASH_WINDOW_FLASH + (at - FLASH_WINDOW_BASE)) / MMU_PAGE;
    }
    if (at - FLASH_IMAGE_BASE < FLASH_IMAGE_SIZE) {
        return MMU_VALID | (at - FLASH_IMAGE_BASE) / MMU_PAGE;
    }
#ifdef PSRAM
    if (at - PSRAM_BASE < PSRAM_SIZE) {
        return MMU_VALID | MMU_PSRAM | (at - PSRAM_BASE) / MMU_PAGE;
    }
#endif
    return 0;
}

static void flash_map(void)
{
    for (uint32_t i = 0; i < MMU_ENTRIES; i++) {
        REG(MMU_ITEM_INDEX) = i;
        REG(MMU_ITEM_CONTENT) = mmu_item(i);
    }
    /* A size of zero empties the whole cache. */
    REG(CACHE_SYNC_ADDR) = 0;
    REG(CACHE_SYNC_SIZE) = 0;
    REG(CACHE_SYNC_CTRL) |= CACHE_INVALIDATE;
    while (!(REG(CACHE_SYNC_CTRL) & CACHE_SYNC_DONE)) {
    }
    REG(CACHE_L1_CACHE_CTRL) &= ~CACHE_SHUT_BUSES;
}

void board_init(void)
{
    REG(PCR_RESET_EVENT_BYPASS) &= ~PCR_BYPASS_SYSTEM_BUS;
    csr_write(mideleg, 0);

    REG(HP_APM_FUNC_CTRL) = 0;
    REG(LP_APM0_FUNC_CTRL) = 0;
    REG(CPU_APM_FUNC_CTRL) = 0;
    REG(LP_APM_FUNC_CTRL) = 0;
    for (uint32_t at = TEE_PERI_FIRST; at <= TEE_PERI_LAST; at += 4) {
        REG(at) = TEE_EVERY_MODE;
    }
    for (uint32_t at = LP_TEE_PERI_FIRST; at <= LP_TEE_PERI_LAST; at += 4) {
        REG(at) = TEE_EVERY_MODE;
    }

    REG(PCR_SPI2_CONF) = PCR_SPI2_CLK_EN | PCR_SPI2_RST_EN;
    REG(PCR_SPI2_CONF) = PCR_SPI2_CLK_EN;
    REG(PCR_SPI2_CLKM_CONF) = PCR_SPI2_CLKM_80M;
    REG(PCR_SARADC_CONF) = PCR_SARADC_CLK_EN | PCR_SARADC_REG_CLK_EN;
    REG(PCR_TSENS_CLK_CONF) = (REG(PCR_TSENS_CLK_CONF) & ~PCR_TSENS_RST_EN) | PCR_TSENS_CLK_XTAL | PCR_TSENS_CLK_EN;
    REG(PCR_RESERVED_14C) = (REG(PCR_RESERVED_14C) & ~PCR_RESERVED_14C_LOW) | PCR_RESERVED_14C_PHY;

    REG(PMU_RF_PWC) = (REG(PMU_RF_PWC) | PMU_RF_I2C_ON) & ~PMU_PERIF_I2C_RSTB;
    REG(PMU_RF_PWC) |= PMU_PERIF_I2C_RSTB;
    REG(PMU_IMM_HP_CK_POWER) = PMU_TIE_HIGH_XPD_BB_I2C;
    REG(LP_AON_SAR_CCT) = (REG(LP_AON_SAR_CCT) & ~LP_AON_SAR2_PWDET_CCT) | LP_AON_SAR2_PWDET_CCT_4;
    REG(PMU_HP_ACTIVE_ICG_MODEM) = PMU_ICG_MODEM_CODE_ACTIVE;
    REG(PMU_IMM_MODEM_ICG) = PMU_UPDATE_ICG_MODEM;
    REG(PMU_IMM_SLEEP_SYSCLK) = PMU_UPDATE_DIG_ICG_SWITCH;

    REG(LPPERI_RNG_CFG) |= RNG_SAMPLE_ENABLE;

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

    flash_map();
}

/*
 * The CSRs user mode writes, measured on the chip; see DESIGN.md, "Boards".
 * By number, since the assembler names none of them.
 * None is a process's to keep, so each is set back whenever another process's thread runs.
 * uintthresh reads its low four bits as ones, and unxti, 0x049 and 0x050 take no write.
 */
void board_user_csrs_switch(struct process *from, struct process *to)
{
    (void)from;
    (void)to;
    csr_clear(mstatus, 0x11); /* UIE and UPIE, which user mode writes as ustatus */
    csr_write(0x005, 0);      /* utvec, whose mode bits stay set */
    csr_write(0x007, 0);      /* utvt */
    csr_write(0x017, 0);      /* undocumented, a base like utvt's */
    csr_write(0x040, 0);      /* uscratch */
    csr_write(0x041, 0);      /* uepc */
    csr_write(0x042, 0);      /* ucause */
    csr_write(0x047, 0);      /* uintthresh */
    csr_write(0x803, 0);      /* cpu_gpio_oen */
    csr_write(0x805, 0);      /* cpu_gpio_out */
}

/* The chip's watchdogs stay off, as board_init leaves them, so a kernel that stopped altogether stays stopped. */
void board_watchdog(uint32_t us)
{
    (void)us;
}
