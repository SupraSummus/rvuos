/*
 * The ESP32-C6's and the ESP32-C5's loop for the DMA test: SPI2 as master, full duplex,
 * sends what the GDMA feeds it out of a pin and takes the pin's level back in as its MISO, through the GPIO matrix,
 * into its buffer, W0 to W15, a transfer of LOOP_WORDS words at a time, which the CPU starts.
 * SPI2 asks the GDMA for no more than its FIFO holds, so the channel reads no further ahead of the CPU than
 * SPI2's FIFO and the GDMA's hold; once nothing feeds it, a transfer sends what it has, the loop's own words.
 * On the C5 a transfer that runs out of words partway takes in one of its own in place of its first,
 * as measured: so a transfer by DMA through the loop is whole transfers of SPI2's, LOOP_GRAIN.
 * The pin is GPIO6, as user/lcd/spi2.c reads the LCD back.
 * SPI2's clock is 10 MHz, its 80 MHz divided by eight, slow enough for a pin's turn round.
 * Register offsets are those of Espressif's ESP-IDF, each chip's spi_reg.h, gpio_reg.h and io_mux_reg.h.
 */

#include "loop.h"
#include "rvuos.h"

#if defined(SPI2_BASE)
#include "pins.h"

#define REG(addr) (*(volatile uint32_t *)(addr))

/*
 * The same pins on both chips: on Waveshare's ESP32-C5 board GPIO6 is the LCD's data line and GPIO23 its select,
 * held high so that the panel takes nothing; the XIAO ESP32-C6 leaves both to its pads.
 */
#define PIN_LOOP     6u
#define PIN_DESELECT 23u

#define SPI_CMD           (SPI2_BASE + 0x00u)
#define SPI_CMD_UPDATE    (1u << 23)
#define SPI_CMD_USR       (1u << 24)
#define SPI_CLOCK         (SPI2_BASE + 0x0cu)
#define SPI_CLOCK_10M     ((7u << 12) | (3u << 6) | (7u << 0)) /* 80 MHz over CLKCNT_N 7 plus one */
#define SPI_USER          (SPI2_BASE + 0x10u)
#define SPI_USER_DOUTDIN  (1u << 0)  /* full duplex */
#define SPI_USER_MOSI     (1u << 27)
#define SPI_USER_MISO     (1u << 28)
#define SPI_USER1         (SPI2_BASE + 0x14u)
#define SPI_USER2         (SPI2_BASE + 0x18u)
#define SPI_MS_DLEN       (SPI2_BASE + 0x1cu)
#define SPI_DMA_CONF      (SPI2_BASE + 0x30u)
#define SPI_DMA_TX_ENA    (1u << 28)
#define SPI_DMA_AFIFO_RST (1u << 31)
#define SPI_W(i)          (SPI2_BASE + 0x98u + 4u * (i))
#define SPI_CLK_GATE      (SPI2_BASE + 0xe8u)
#define SPI_CLK_GATE_ON   7u /* the clock, the master's clock active, and that clock the 80 MHz */

/* Polls before a transfer is given up on, far more than one of LOOP_WORDS words takes at 10 MHz. */
#define SPI_POLLS 100000u

static void fail(const char *what)
{
    rv_puts(BOOT_CAP_PRINT, "dma: ");
    rv_puts(BOOT_CAP_PRINT, what);
    rv_puts(BOOT_CAP_PRINT, ": FAILED\n");
    rv_halt(BOOT_CAP_HALT, 3);
}

static bool settle(uint32_t bit)
{
    for (uint32_t i = 0; i < SPI_POLLS; i++) {
        if (!(REG(SPI_CMD) & bit)) {
            return true;
        }
    }
    return false;
}

void loop_open(struct loop *loop, uint32_t region)
{
    if (rv_process_install(BOOT_CAP_PROCESS, region, BOOT_CAP_SPI2, RIGHT_R | RIGHT_W) != KERR_OK ||
        rv_process_install(BOOT_CAP_PROCESS, region + 1u, BOOT_CAP_IO_MUX, RIGHT_R | RIGHT_W) != KERR_OK ||
        rv_process_install(BOOT_CAP_PROCESS, region + 2u, BOOT_CAP_GPIO, RIGHT_R | RIGHT_W) != KERR_OK) {
        fail("map SPI2 and the pins");
    }
    REG(GPIO_OUT_W1TS) = 1u << PIN_DESELECT;
    REG(GPIO_OUT_SEL(PIN_DESELECT)) = SIG_GPIO_OUT | GPIO_OE_FROM_ENABLE;
    REG(IO_MUX_PIN(PIN_DESELECT)) = IO_MUX_MATRIX(2u);
    REG(GPIO_ENABLE_W1TS) = 1u << PIN_DESELECT;
    REG(GPIO_OUT_SEL(PIN_LOOP)) = SIG_FSPID | GPIO_OE_FROM_ENABLE;
    REG(IO_MUX_PIN(PIN_LOOP)) = IO_MUX_MATRIX(2u) | IO_MUX_INPUT;
    REG(GPIO_ENABLE_W1TS) = 1u << PIN_LOOP;
    REG(GPIO_IN_SEL(SIG_FSPIQ)) = PIN_LOOP | GPIO_IN_MATRIX;

    REG(SPI_CLK_GATE) = SPI_CLK_GATE_ON;
    REG(SPI_USER) = SPI_USER_DOUTDIN | SPI_USER_MOSI | SPI_USER_MISO;
    REG(SPI_USER1) = 0;
    REG(SPI_USER2) = 0;
    REG(SPI_CLOCK) = SPI_CLOCK_10M;
    REG(SPI_MS_DLEN) = LOOP_WORDS * 32u - 1u;
    REG(SPI_CMD) = SPI_CMD_UPDATE;
    if (!settle(SPI_CMD_UPDATE)) {
        fail("SPI2 takes its configuration");
    }
    loop_ready(loop);
}

/* SPI2's FIFO emptied of what an earlier transfer's channel left in it, and the words taken forgotten. */
void loop_ready(struct loop *loop)
{
    REG(SPI_DMA_CONF) = SPI_DMA_TX_ENA | SPI_DMA_AFIFO_RST;
    REG(SPI_DMA_CONF) = SPI_DMA_TX_ENA;
    loop->have = loop->next = 0;
}

bool loop_take(struct loop *loop, uint32_t *word)
{
    if (loop->next == loop->have) {
        REG(SPI_CMD) = SPI_CMD_UPDATE;
        if (!settle(SPI_CMD_UPDATE)) {
            return false;
        }
        REG(SPI_CMD) = SPI_CMD_USR;
        if (!settle(SPI_CMD_USR)) {
            return false;
        }
        for (uint32_t i = 0; i < LOOP_WORDS; i++) {
            loop->words[i] = REG(SPI_W(i));
        }
        loop->have = LOOP_WORDS;
        loop->next = 0;
    }
    *word = loop->words[loop->next++];
    return true;
}

#endif
