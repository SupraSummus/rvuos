/*
 * The ESP32-C5's MSPI: the flash, which the cache reads through SPI0.
 * SPI1 takes the commands the kernel sends it at boot.
 *
 * The ROM's boot leaves the MSPI on the 48 MHz crystal and the flash read dual I/O, whatever the image's header asks.
 * Once the SPLL is set, the kernel moves the MSPI to PLL_F480M divided by six, 80 MHz, the flash's clock the whole of it,
 * and the reads to quad I/O, command 0xEB, where the flash's QE bit is set, as ESP-IDF's bootloader has the ROM do.
 * On a XIAO's Puya flash a miss of the cache went from 571 cycles to 213.
 * ESP-IDF tunes the input's timing above 40 MHz; the reads were checked right at 80 MHz with none.
 * A reset keeps PCR's clock, so the ROM's next boot reads the flash at 80 MHz.
 *
 * Whoever holds the IO MUX can take the flash's pins, see board.h.
 */

#include <stdint.h>

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

void mspi_init(void)
{
    flash_set();
}
