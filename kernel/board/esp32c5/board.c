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
 * The MMU's only valid entries are the image's 64 KiB pages, see board.h, which the kernel sets,
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
 */
#define TEE_PERI_FIRST       0x60098088u /* UART0's */
#define TEE_PERI_LAST        0x60098140u /* the bit scrambler's, past SPI2's */
#define LP_TEE_PERI_FIRST    0x600B3404u /* the eFuse's */
#define LP_TEE_PERI_LAST     0x600B344Cu /* LP_APM's */
#define TEE_EVERY_MODE       0xFFu

/* SPI2's clocks and reset, in PCR. */
#define PCR_SPI2_CONF        0x600960C4u
#define PCR_SPI2_CLK_EN      (1u << 0) /* its registers' clock */
#define PCR_SPI2_RST_EN      (1u << 1)
#define PCR_SPI2_CLKM_CONF   0x600960C8u
#define PCR_SPI2_CLKM_80M    ((1u << 22) | (1u << 20) | (1u << 12)) /* CLKM_EN, PLL_F160M, divided by DIV_NUM 1 plus one */

/* The MMU, which maps 64 KiB pages of flash through the cache from 0x42000000, one entry each, and the cache. */
#define MMU_ITEM_CONTENT     0x6000237Cu
#define MMU_ITEM_INDEX       0x60002380u
#define MMU_BASE             0x42000000u
#define MMU_ENTRIES          512u
#define MMU_PAGE             0x10000u
#define MMU_VALID            (1u << 10)
#define CACHE_L1_CACHE_CTRL  (0x600C8000u + 0x04u)
#define CACHE_SHUT_BUSES     0x3u /* IBUS and DBUS */
#define CACHE_SYNC_CTRL      (0x600C8000u + 0x94u)
#define CACHE_SYNC_ADDR      (0x600C8000u + 0x9cu)
#define CACHE_SYNC_SIZE      (0x600C8000u + 0xa0u)
#define CACHE_INVALIDATE     (1u << 0)
#define CACHE_SYNC_DONE      (1u << 4)

_Static_assert(FLASH_IMAGE_BASE % MMU_PAGE == 0 && FLASH_IMAGE_SIZE % MMU_PAGE == 0 &&
                   USER_CODE_FLASH + USER_CODE_SIZE <= INPUT_FLASH && INPUT_FLASH + INPUT_SIZE <= FLASH_IMAGE_SIZE,
               "the image is whole pages, the root task's code and input in it");
_Static_assert(FLASH_IMAGE_BASE >= MMU_BASE && FLASH_IMAGE_BASE + FLASH_IMAGE_SIZE <= MMU_BASE + MMU_PAGE * MMU_ENTRIES,
               "the image lies in the MMU's space");

/* The flash page entry i of the MMU shows, or 0 for none. */
static uint32_t mmu_item(uint32_t i)
{
    uint32_t at = MMU_BASE + i * MMU_PAGE;
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
    REG(CACHE_SYNC_ADDR) = 0;
    REG(CACHE_SYNC_SIZE) = 0;
    REG(CACHE_SYNC_CTRL) |= CACHE_INVALIDATE;
    while (!(REG(CACHE_SYNC_CTRL) & CACHE_SYNC_DONE)) {
    }
    REG(CACHE_L1_CACHE_CTRL) &= ~CACHE_SHUT_BUSES;
}

void board_init(void)
{
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
