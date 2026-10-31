/*
 * The ESP32-C5's MSPI: the flash, which the cache reads through SPI0, and, with PSRAM=1, the PSRAM beside it.
 * SPI1 takes the commands the kernel sends either of them at boot.
 *
 * The ROM's boot leaves the MSPI on the 48 MHz crystal and the flash read dual I/O, whatever the image's header asks.
 * Once the SPLL is set, the kernel moves the MSPI to PLL_F480M divided by six, 80 MHz, the flash's clock the whole of it,
 * and the reads to quad I/O, command 0xEB, where the flash's QE bit is set, as ESP-IDF's bootloader has the ROM do.
 * On a XIAO's Puya flash a miss of the cache went from 571 cycles to 213.
 * ESP-IDF tunes the input's timing above 40 MHz; the reads were checked right at 80 MHz with none.
 * A reset keeps PCR's clock, so the ROM's next boot reads the flash at 80 MHz.
 *
 * The PSRAM of an ESP32-C5HR8, 8 MiB of AP Memory's quad PSRAM, is on CS1, GPIO15.
 * It comes up as ESP-IDF's esp_psram_impl_ap_quad.c brings it:
 * 0xF5 on four lines leaves QPI, where a reset leaves it, then 0x9F reads its ID, 0x66 and 0x99 reset it, and 0x35 enters QPI.
 * SPI0 then reads it with 0xEB and writes it with 0x38, all on four lines, at 40 MHz, which needs no tuning.
 * The MMU shows it from PSRAM_BASE, see board.c.
 * The core's PMA refuses an address no entry covers, so the kernel adds one over the PSRAM.
 *
 * Whoever holds the IO MUX can take the pins of either, see board.h.
 */

#include <stdint.h>

#include "csr.h"
#include "kernel.h"
#include "layout.h"
#include "mspi.h"

#define REG(addr) (*(volatile uint32_t *)(addr))

/* The MSPI's core clock, in PCR: its source and divider. */
#define PCR_MSPI_CLK_CONF    0x6009601Cu
#define PCR_MSPI_CLK_EN      (1u << 10)
#define PCR_MSPI_PLL_F480M   (2u << 8)
#define PCR_MSPI_DIV(n)      ((n) - 1u)

/* SPI0, the cache's, and SPI1, which takes commands, and their registers as ESP-IDF's soc/esp32c5 names them. */
#define SPI0                 0x60002000u
#define SPI1                 0x60003000u
#define SPI_CMD(spi)         ((spi) + 0x00u)
#define SPI_ADDR(spi)        ((spi) + 0x04u)
#define SPI_CTRL(spi)        ((spi) + 0x08u)
#define SPI_CLOCK(spi)       ((spi) + 0x14u)
#define SPI_USER(spi)        ((spi) + 0x18u)
#define SPI_USER1(spi)       ((spi) + 0x1cu)
#define SPI_USER2(spi)       ((spi) + 0x20u)
#define SPI_MISO_DLEN(spi)   ((spi) + 0x28u)
#define SPI_MISC(spi)        ((spi) + 0x34u)
#define SPI_W0(spi)          ((spi) + 0x58u)
#define SPI_CMD_USR          (1u << 18)       /* send the command USER describes; clears once sent */
#define SPI_CTRL_FCMD_QUAD   (1u << 8)
#define SPI_CTRL_FREAD_DIO   (1u << 23)
#define SPI_CTRL_FREAD_QIO   (1u << 24)
/* The lines every phase takes: FCMD_QUAD, FASTRD_MODE, FREAD_DUAL, FREAD_QUAD, FREAD_DIO and FREAD_QIO. */
#define SPI_CTRL_LINES       (SPI_CTRL_FCMD_QUAD | (1u << 13) | (1u << 14) | (1u << 20) | SPI_CTRL_FREAD_DIO | SPI_CTRL_FREAD_QIO)
#define SPI_CLOCK_WHOLE      (1u << 31)       /* the device's clock is the core's */
#define SPI_CLOCK_QUARTER    (3u | (1u << 8) | (3u << 16))
#define SPI_USER_COMMAND     (1u << 31)
#define SPI_USER_ADDR        (1u << 30)
#define SPI_USER_MISO        (1u << 28)
#define SPI_USER1_ADDR_BITS  (0x3fu << 26)    /* the address phase's bits, less one */
#define SPI_USER1_DUMMY      0x3fu            /* the dummy cycles, less one */
#define SPI_USER1_QIO        ((31u << 26) | 3u)
#define SPI_USER2_COMMAND    0xffffu
#define SPI_USER2_8BIT       (7u << 28)
#define SPI_MISC_CS          3u               /* CS0_DIS and CS1_DIS */
#define SPI_SPINS            100000u          /* a command takes microseconds; the bound only keeps a dead bus from hanging the boot */

#define CS_FLASH             2u               /* CS1_DIS: the flash's CS0 alone */
#define CS_PSRAM             1u               /* CS0_DIS: the PSRAM's CS1 alone */

#define FLASH_READ_QIO       0xEBu
#define FLASH_RDSR2          0x35u
#define FLASH_SR2_QE         (1u << 1)
#define IO_MUX_PIN(n)        (0x60090000u + 4u * (n))
#define IO_MUX_FUNCTION      (7u << 12)
#define FLASH_WP_PIN         18u
#define FLASH_HD_PIN         20u

/*
 * Send one command through SPI1, on one line, or on four for the command alone with quad,
 * to the chip select cs: an address of zero of addr_bits bits, then a read of rx_bits, which it returns.
 * It runs at a quarter of the MSPI's clock, and sets back every register it changed, which are the flash's otherwise.
 */
static uint32_t spi1_command(uint32_t cs, uint32_t command, bool quad, uint32_t addr_bits, uint32_t rx_bits)
{
    uint32_t ctrl = REG(SPI_CTRL(SPI1)), clock = REG(SPI_CLOCK(SPI1)), user = REG(SPI_USER(SPI1));
    uint32_t user1 = REG(SPI_USER1(SPI1)), user2 = REG(SPI_USER2(SPI1)), dlen = REG(SPI_MISO_DLEN(SPI1));
    uint32_t misc = REG(SPI_MISC(SPI1));
    REG(SPI_CTRL(SPI1)) = (ctrl & ~SPI_CTRL_LINES) | (quad ? SPI_CTRL_FCMD_QUAD : 0);
    REG(SPI_CLOCK(SPI1)) = SPI_CLOCK_QUARTER;
    REG(SPI_USER(SPI1)) = SPI_USER_COMMAND | (addr_bits ? SPI_USER_ADDR : 0) | (rx_bits ? SPI_USER_MISO : 0);
    REG(SPI_USER1(SPI1)) = (user1 & ~SPI_USER1_ADDR_BITS) | (addr_bits ? (addr_bits - 1u) << 26 : 0);
    REG(SPI_USER2(SPI1)) = SPI_USER2_8BIT | command;
    REG(SPI_ADDR(SPI1)) = 0;
    REG(SPI_MISO_DLEN(SPI1)) = rx_bits ? rx_bits - 1u : 0;
    REG(SPI_MISC(SPI1)) = (misc & ~SPI_MISC_CS) | cs;
    REG(SPI_W0(SPI1)) = 0;
    REG(SPI_CMD(SPI1)) = SPI_CMD_USR;
    for (uint32_t n = 0; n < SPI_SPINS && (REG(SPI_CMD(SPI1)) & SPI_CMD_USR); n++) {
    }
    uint32_t read = REG(SPI_W0(SPI1));
    REG(SPI_CTRL(SPI1)) = ctrl;
    REG(SPI_CLOCK(SPI1)) = clock;
    REG(SPI_USER(SPI1)) = user;
    REG(SPI_USER1(SPI1)) = user1;
    REG(SPI_USER2(SPI1)) = user2;
    REG(SPI_MISO_DLEN(SPI1)) = dlen;
    REG(SPI_MISC(SPI1)) = misc;
    return read;
}

/* The flash's reads quad I/O, if its QE bit is set, and the MSPI's clock 80 MHz, the flash's the whole of it. */
static void flash_set(void)
{
    if (spi1_command(CS_FLASH, FLASH_RDSR2, false, 0, 8u) & FLASH_SR2_QE) {
        REG(IO_MUX_PIN(FLASH_WP_PIN)) &= ~IO_MUX_FUNCTION;
        REG(IO_MUX_PIN(FLASH_HD_PIN)) &= ~IO_MUX_FUNCTION;
        for (uint32_t spi = SPI0; spi <= SPI1; spi += SPI1 - SPI0) {
            REG(SPI_CTRL(spi)) = (REG(SPI_CTRL(spi)) & ~SPI_CTRL_FREAD_DIO) | SPI_CTRL_FREAD_QIO;
            REG(SPI_USER1(spi)) = (REG(SPI_USER1(spi)) & ~(SPI_USER1_ADDR_BITS | SPI_USER1_DUMMY)) | SPI_USER1_QIO;
            REG(SPI_USER2(spi)) = (REG(SPI_USER2(spi)) & ~SPI_USER2_COMMAND) | FLASH_READ_QIO;
        }
    }
    REG(SPI_CLOCK(SPI0)) = SPI_CLOCK_WHOLE;
    REG(SPI_CLOCK(SPI1)) = SPI_CLOCK_WHOLE;
    REG(PCR_MSPI_CLK_CONF) = PCR_MSPI_CLK_EN | PCR_MSPI_PLL_F480M | PCR_MSPI_DIV(6u);
}

#ifdef PSRAM

/* SPI0's registers for the PSRAM. */
#define SPI0_CACHE_SCTRL     (SPI0 + 0x40u)
#define SPI0_SRAM_DRD_CMD    (SPI0 + 0x48u)
#define SPI0_SRAM_DWR_CMD    (SPI0 + 0x4cu)
#define SPI0_SRAM_CLK        (SPI0 + 0x50u)
#define SPI0_SMEM_ECC_CTRL   (SPI0 + 0x174u)
#define SPI0_SMEM_AC         (SPI0 + 0x1a0u)
/* CACHE_SCTRL: dual I/O off; quad I/O, a read's dummy phase, the cache's own read and write commands on; the phases' lengths. */
#define SCTRL_SRAM_DIO       (1u << 1)
#define SCTRL_SRAM_QIO       (1u << 2)
#define SCTRL_RD_DUMMY       (1u << 4)
#define SCTRL_USR_RCMD       (1u << 5)
#define SCTRL_RDUMMY(n)      (((n) - 1u) << 6)
#define SCTRL_RDUMMY_MASK    (0x3fu << 6)
#define SCTRL_ADDR_BITS(n)   (((n) - 1u) << 14)
#define SCTRL_ADDR_MASK      (0x3fu << 14)
#define SCTRL_USR_WCMD       (1u << 20)
#define SRAM_QIO_READ        (SPI_USER2_8BIT | 0xEBu)
#define SRAM_QIO_WRITE       (SPI_USER2_8BIT | 0x38u)
#define SRAM_CLK_HALF        (1u | (1u << 16)) /* the MSPI's clock divided by two */
#define SMEM_PAGE_SIZE_MASK  (3u << 18)
#define SMEM_PAGE_1K         (2u << 18)        /* a transfer splits at the PSRAM's 1 KiB pages */
#define SMEM_CS_SETUP        (1u << 0)
#define SMEM_CS_HOLD         (1u << 1)
#define SMEM_SPLIT_TRANS     (1u << 31)

#define PSRAM_EXIT_QPI       0xF5u
#define PSRAM_READ_ID        0x9Fu
#define PSRAM_RESET_ENABLE   0x66u
#define PSRAM_RESET          0x99u
#define PSRAM_ENTER_QPI      0x35u
#define PSRAM_ID_AP_KGD      0x5D0Du /* the ID's first two bytes: AP Memory's, and a known good die */
#define PSRAM_CS_PIN         15u

/* Espressif's PMA entry 0, pmacfg0 and pmaaddr0: an address NAPOT-coded as pmpaddr's, its bits as pmpcfg's. */
#define PMA_EN               (1u << 0)
#define PMA_X                (1u << 2)
#define PMA_W                (1u << 3)
#define PMA_R                (1u << 4)
#define PMA_NAPOT            0xC0000000u
_Static_assert((PSRAM_SIZE & (PSRAM_SIZE - 1u)) == 0 && PSRAM_BASE % PSRAM_SIZE == 0, "the PSRAM is a NAPOT block");

static void psram_init(void)
{
    REG(IO_MUX_PIN(PSRAM_CS_PIN)) &= ~IO_MUX_FUNCTION;
    REG(SPI0_SMEM_AC) |= SMEM_CS_SETUP | SMEM_CS_HOLD | SMEM_SPLIT_TRANS;
    REG(SPI0_SMEM_ECC_CTRL) = (REG(SPI0_SMEM_ECC_CTRL) & ~SMEM_PAGE_SIZE_MASK) | SMEM_PAGE_1K;

    spi1_command(CS_PSRAM, PSRAM_EXIT_QPI, true, 0, 0);
    if ((spi1_command(CS_PSRAM, PSRAM_READ_ID, false, 24u, 24u) & 0xffffu) != PSRAM_ID_AP_KGD) {
        kpanic("no PSRAM answers, which a kernel built with PSRAM=1 grants");
    }
    spi1_command(CS_PSRAM, PSRAM_RESET_ENABLE, false, 0, 0);
    spi1_command(CS_PSRAM, PSRAM_RESET, false, 0, 0);
    spi1_command(CS_PSRAM, PSRAM_ENTER_QPI, false, 0, 0);

    uint32_t sctrl = REG(SPI0_CACHE_SCTRL) & ~(SCTRL_SRAM_DIO | SCTRL_RDUMMY_MASK | SCTRL_ADDR_MASK);
    REG(SPI0_CACHE_SCTRL) = sctrl | SCTRL_SRAM_QIO | SCTRL_RD_DUMMY | SCTRL_USR_RCMD | SCTRL_RDUMMY(6u) |
                            SCTRL_ADDR_BITS(24u) | SCTRL_USR_WCMD;
    REG(SPI0_SRAM_DRD_CMD) = SRAM_QIO_READ;
    REG(SPI0_SRAM_DWR_CMD) = SRAM_QIO_WRITE;
    REG(SPI0_SRAM_CLK) = SRAM_CLK_HALF;

    csr_write(0xBD0, (PSRAM_BASE | (PSRAM_SIZE / 2u - 1u)) >> 2);
    csr_write(0xBC0, PMA_NAPOT | PMA_R | PMA_W | PMA_X | PMA_EN);
}

#endif

void mspi_init(void)
{
    flash_set();
#ifdef PSRAM
    psram_init();
#endif
}
