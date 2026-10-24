/*
 * The board's bus.h: SPI2 and the pins of Waveshare's ESP32-C5-LCD-1.47, as its own support package names them.
 *
 * SPI2 sends what the CPU writes into its 64-byte buffer, a transfer at a time, with no DMA:
 * the GDMA, which would reach all of RAM, is not the program's, see the kernel's board.h.
 * The select is a plain pin, held low across a command and all its data,
 * so the data may span as many transfers as it takes.
 * The clock and data reach SPI2 through the GPIO matrix, since the IO MUX's own SPI pins are the other way round.
 * The kernel sets SPI2's master clock, 80 MHz, which this halves for the bus.
 * The registers are mapped at their own addresses, by the root task, before lcd_bus_init.
 */

#include <stdint.h>

#include "bus.h"
#include "spi2.h"
#include "devices.h"
#include "rvuos.h"

#define REG(addr) (*(volatile uint32_t *)(addr))

/* The board's pins. */
#define PIN_MOSI 6
#define PIN_CLK  7
#define PIN_BL   10 /* the backlight, lit while high */
#define PIN_CS   23
#define PIN_DC   24 /* low for a command, high for its data */
#define PIN_RST  26 /* the controller's reset, while low */

/* The GPIO matrix: levels, output enables, and each pin's output signal. */
#define GPIO_OUT_W1TS    (GPIO_BASE + 0x08u)
#define GPIO_OUT_W1TC    (GPIO_BASE + 0x0cu)
#define GPIO_ENABLE_W1TS (GPIO_BASE + 0x38u)
#define GPIO_OUT_SEL(n)  (GPIO_BASE + 0xad4u + 4u * (n))
#define GPIO_OE_FROM_ENABLE (1u << 10) /* the pin is driven while its GPIO_ENABLE bit is set */
#define SIG_GPIO_OUT     256u          /* the pin's level is its GPIO_OUT bit */
#define SIG_FSPICLK      56u
#define SIG_FSPID        58u

/* The IO MUX: each pin's function, 1 the GPIO matrix's, and its drive strength, of 0 to 3. */
#define IO_MUX_PIN(n)    (IO_MUX_BASE + 4u * (n))
#define IO_MUX_MATRIX(drive) ((1u << 12) | ((drive) << 10))
#define DRIVE_DEFAULT    2u

/* SPI2, as master, sending from its buffer W0 to W15. */
#define SPI_CMD          (SPI2_BASE + 0x00u)
#define SPI_CMD_UPDATE   (1u << 23) /* the controller takes the registers below; clears itself */
#define SPI_CMD_USR      (1u << 24) /* the transfer starts; clears itself once it is done */
#define SPI_CLOCK        (SPI2_BASE + 0x0cu)
#define SPI_CLOCK_40M    ((1u << 12) | (0u << 6) | (1u << 0)) /* the kernel's 80 MHz over CLKCNT_N 1 plus one */
#define SPI_CLOCK_80M    (1u << 31)                           /* CLK_EQU_SYSCLK: the kernel's 80 MHz itself */
#define SPI_USER         (SPI2_BASE + 0x10u)
#define SPI_USER_MOSI    (1u << 27) /* the transfer is data out, and nothing more */
#define SPI_USER1        (SPI2_BASE + 0x14u)
#define SPI_USER2        (SPI2_BASE + 0x18u)
#define SPI_MS_DLEN      (SPI2_BASE + 0x1cu)
#define SPI_DMA_CONF     (SPI2_BASE + 0x30u)
#define SPI_BUF_AFIFO_RST (1u << 30)
#define SPI_W(i)         (SPI2_BASE + 0x98u + 4u * (i))
#define SPI_BUF_BYTES    64u

/* Polls before a wait for SPI2 gives up on it, far more than a transfer of the buffer takes at 40 MHz. */
#define SPI_POLLS 100000u

static void pin(unsigned n, int high)
{
    REG(high ? GPIO_OUT_W1TS : GPIO_OUT_W1TC) = 1u << n;
}

/* Wait until SPI2 clears bit in its command register. */
static void settle(uint32_t bit, const char *what)
{
    for (uint32_t i = 0; i < SPI_POLLS; i++) {
        if (!(REG(SPI_CMD) & bit)) {
            return;
        }
    }
    rv_puts(BOOT_CAP_PRINT, "lcd: ");
    rv_puts(BOOT_CAP_PRINT, what);
    rv_puts(BOOT_CAP_PRINT, ": FAILED\n");
    rv_halt(BOOT_CAP_HALT, 1);
}

/* Send what the buffer holds, n bytes of it, at most SPI_BUF_BYTES. */
static void send(uint32_t n)
{
    REG(SPI_DMA_CONF) = SPI_BUF_AFIFO_RST;
    REG(SPI_MS_DLEN) = n * 8 - 1;
    REG(SPI_CMD) = SPI_CMD_UPDATE;
    settle(SPI_CMD_UPDATE, "SPI2 takes its registers");
    REG(SPI_CMD) = SPI_CMD_USR;
    settle(SPI_CMD_USR, "SPI2 sends");
}

/*
 * Load n bytes, at most SPI_BUF_BYTES, into the buffer, which sends each word's low byte first:
 * a little-endian load of four bytes is the word that sends them in order.
 */
static void load(const uint8_t *bytes, uint32_t n)
{
    uint32_t words = n / 4;
    if (((uintptr_t)bytes & 3u) == 0) {
        const uint32_t *w = (const uint32_t *)(const void *)bytes;
        for (uint32_t i = 0; i < words; i++) {
            REG(SPI_W(i)) = w[i];
        }
    } else {
        for (uint32_t i = 0; i < words; i++) {
            const uint8_t *b = bytes + 4 * i;
            REG(SPI_W(i)) = b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
        }
    }
    if (n % 4 != 0) {
        uint32_t last = 0;
        for (uint32_t i = words * 4; i < n; i++) {
            last |= (uint32_t)bytes[i] << (8 * (i % 4));
        }
        REG(SPI_W(words)) = last;
    }
}

static const uint8_t outputs[] = { PIN_MOSI, PIN_CLK, PIN_BL, PIN_CS, PIN_DC, PIN_RST };

void lcd_bus_init(void)
{
    /* Idle levels before the pins are driven: deselected, out of reset, data, and dark. */
    pin(PIN_CS, 1);
    pin(PIN_RST, 1);
    pin(PIN_DC, 1);
    pin(PIN_BL, 0);
    for (uint32_t i = 0; i < sizeof(outputs); i++) {
        uint32_t n = outputs[i];
        uint32_t signal = n == PIN_CLK ? SIG_FSPICLK : n == PIN_MOSI ? SIG_FSPID : SIG_GPIO_OUT;
        REG(GPIO_OUT_SEL(n)) = signal | GPIO_OE_FROM_ENABLE;
        REG(IO_MUX_PIN(n)) = IO_MUX_MATRIX(DRIVE_DEFAULT);
        REG(GPIO_ENABLE_W1TS) = 1u << n;
    }

    REG(SPI_USER) = SPI_USER_MOSI;
    REG(SPI_USER1) = 0;
    REG(SPI_USER2) = 0;
    REG(SPI_CLOCK) = SPI_CLOCK_40M;
    REG(SPI_CMD) = SPI_CMD_UPDATE;
    settle(SPI_CMD_UPDATE, "SPI2 takes its configuration");
}

void lcd_bus_command(uint8_t command)
{
    pin(PIN_CS, 0);
    pin(PIN_DC, 0);
    REG(SPI_W(0)) = command;
    send(1);
    pin(PIN_DC, 1);
}

void lcd_bus_data(const uint8_t *bytes, uint32_t n)
{
    while (n > 0) {
        uint32_t chunk = n < SPI_BUF_BYTES ? n : SPI_BUF_BYTES;
        load(bytes, chunk);
        send(chunk);
        bytes += chunk;
        n -= chunk;
    }
}

void lcd_bus_end(void)
{
    pin(PIN_CS, 1);
}

void lcd_bus_reset(int low)
{
    pin(PIN_RST, !low);
}

void lcd_bus_backlight(int on)
{
    pin(PIN_BL, on);
}

void spi2_configure(uint32_t mhz, uint32_t drive)
{
    for (uint32_t i = 0; i < sizeof(outputs); i++) {
        REG(IO_MUX_PIN(outputs[i])) = IO_MUX_MATRIX(drive & 3u);
    }
    REG(SPI_CLOCK) = mhz == 80 ? SPI_CLOCK_80M : SPI_CLOCK_40M;
    REG(SPI_CMD) = SPI_CMD_UPDATE;
    settle(SPI_CMD_UPDATE, "SPI2 takes its clock");
}
