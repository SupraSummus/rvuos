/*
 * Machine timer of the ESP32-C5: the CLINT's mtime and mtimecmp, at 0x20000000.
 *
 * The CLINT is laid out as SiFive's, with a control word beside mtimecmp that starts the counter,
 * which the ROM leaves off; its compare raises the timer interrupt, a level while mtime has reached mtimecmp,
 * which the CLIC takes as interrupt 7 once its own enable is set, and clint.c drives it.
 * It counts at the CPU clock, so the kernel measures it at boot against the system timer,
 * which runs at 16 MHz off the 48 MHz crystal whatever the CPU clock is.
 *
 * The kernel sets the CPU clock here, before measuring it, rather than keep the one it finds,
 * since a reset keeps PCR's, as on the ESP32-C6, so it is whatever the last program left.
 * 160 MHz is PLL_F160M, the SPLL's 480 divided by three, and the AHB a quarter of that, as ESP-IDF sets it.
 * The SPLL's dividers outlive a reset and the ROM's boot from flash, and the board's boots found them as for
 * a 40 MHz crystal: 576 MHz on its 48, PLL_F160M at 192, where ESP-IDF, and the radio's baseband with it, wants 480.
 * So sspll_set writes ESP-IDF's dividers first, over the modem's analog I2C master, the CPU on the crystal meanwhile.
 * ESP-IDF's 240 MHz takes PLL_F240M, with its voltage raised first,
 * and on this revision a change from PLL_F160M to PLL_F240M can stop the root clock, IDF-11064,
 * which 160 MHz never asks for.
 * The boot's log says the clock set and the one found, both measured.
 */

#include <stdint.h>

#include "clint.h"
#include "kernel.h"
#include "layout.h"
#include "timer.h"

#define CLINT_BASE     0x20000000u
#define CLINT_TIMECTL  (CLINT_BASE + 0x4010u)
_Static_assert(CLINT_MTIME == CLINT_BASE + 0xBFF8u && CLINT_MTIMECMP == CLINT_BASE + 0x4000u,
               "board.h names this CLINT's registers");
#define TIMECTL_COUNTER_EN (1u << 0)

/* The system timer's unit 0, which counts from reset. */
#define SYSTIMER_BASE     0x6000A000u
#define SYSTIMER_UNIT0_OP (SYSTIMER_BASE + 0x04u)
#define SYSTIMER_UNIT0_HI (SYSTIMER_BASE + 0x40u)
#define SYSTIMER_UNIT0_LO (SYSTIMER_BASE + 0x44u)
#define SYSTIMER_UPDATE   (1u << 30) /* latch the count into HI and LO */
#define SYSTIMER_VALID    (1u << 29) /* the latched count is there to read */
#define SYSTIMER_HZ       16000000u

/* The CPU's clock, in PCR: its source, and the dividers below it for the CPU and the AHB, which take effect on an update. */
#define PCR_SYSCLK_CONF      0x60096110u
#define PCR_SOC_CLK_SEL      (3u << 16)
#define PCR_SOC_CLK_PLL_F160 (2u << 16)
#define PCR_CPU_FREQ_CONF    0x60096118u
#define PCR_CPU_UNDIVIDED    0u        /* CPU_DIV_NUM 0 */
#define PCR_AHB_FREQ_CONF    0x6009611cu
#define PCR_AHB_DIV4         3u        /* AHB_DIV_NUM 3 */
#define PCR_BUS_CLK_UPDATE   0x60096144u
#define PCR_BUS_CLOCK_UPDATE (1u << 0) /* clears itself once the new dividers and source took */

#define PCR_SOC_CLK_XTAL     (0u << 16)

/* The SPLL's analog block, 0x66 on the analog I2C, and its registers as ESP-IDF's soc/regi2c_bbpll.h names them. */
#define I2C_MST_CTRL1        0x600AF804u /* the master's second command word, which reaches the SPLL's block */
#define I2C_MST_ANA_CONF0    0x600AF818u /* the SPLL's calibration: stop forced high bit 2, low bit 3; done bit 24 */
#define I2C_MST_ANA_CONF1    0x600AF81Cu /* the read mask, the blocks a read leaves out, inverted */
#define I2C_MST_ANA_CONF2    0x600AF820u /* which blocks are the second word's: the PHY's library's routing */
#define I2C_MST_ROUTE_MASK   0xfffe000fu
#define I2C_MST_ROUTE        0x00019c10u
#define I2C_MST_BBPLL_READ   (0x20u << 2) /* the SPLL block's bit of the read mask, from the ROM's table */
#define I2C_MST_START        (1u << 26)
#define I2C_MST_WRITE        (1u << 24)
#define I2C_MST_BUSY         (1u << 25)
#define I2C_BBPLL            0x66u
#define CAL_STOP_HIGH        (1u << 2)
#define CAL_STOP_LOW         (1u << 3)
#define CAL_DONE             (1u << 24)
#define I2C_SPINS            100000u /* a command takes some microseconds; the bound only keeps a dead bus from hanging the boot */
#define CAL_SPINS            10000000u
/*
 * The master's clock, in MODEM_SYSCON and MODEM_LPCON, and the SPLL's and its bus's power, in the PMU.
 * The kernel forces the master's clock on for the change and lets it go after,
 * since its enable and its gating maps, which the PHY's library sets later, do not reach it at boot:
 * without the force the master's words read zero and take no write, as a boot from flash measured.
 */
#define MODEM_SYSCON_CLK_CONF     0x600A9C04u
#define CLK_I2C_MST_SEL_160M      (1u << 12)
#define MODEM_LPCON_CLK_CONF_FORCE 0x600AF01Cu
#define CLK_I2C_MST_FO            (1u << 2)
#define PMU_IMM_HP_CK_POWER       0x600B00CCu
#define PMU_TIE_HIGH_BBPLL        ((1u << 25) | (1u << 28) | (1u << 29) | (1u << 30)) /* its ICG, BB_I2C, BBPLL_I2C, BBPLL */

#define REG(addr) (*(volatile uint32_t *)(addr))

static void i2c_wait(void)
{
    for (uint32_t n = 0; n < I2C_SPINS && (REG(I2C_MST_CTRL1) & I2C_MST_BUSY); n++) {
    }
}

static uint8_t bbpll_read(uint8_t reg)
{
    REG(I2C_MST_ANA_CONF1) = ~I2C_MST_BBPLL_READ;
    REG(I2C_MST_CTRL1) = I2C_BBPLL | (uint32_t)reg << 8 | I2C_MST_START;
    i2c_wait();
    return (uint8_t)(REG(I2C_MST_CTRL1) >> 16);
}

static void bbpll_write(uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t v = (uint8_t)((bbpll_read(reg) & ~mask) | (value & mask));
    REG(I2C_MST_CTRL1) = I2C_BBPLL | (uint32_t)reg << 8 | (uint32_t)v << 16 | I2C_MST_START | I2C_MST_WRITE;
    i2c_wait();
}

static uint64_t systimer_read(void)
{
    REG(SYSTIMER_UNIT0_OP) = SYSTIMER_UPDATE;
    while ((REG(SYSTIMER_UNIT0_OP) & SYSTIMER_VALID) == 0) {
    }
    return ((uint64_t)REG(SYSTIMER_UNIT0_HI) << 32) | REG(SYSTIMER_UNIT0_LO);
}

/* A millisecond of the system timer, counted in mtime: a millisecond's length at the CPU clock. */
static uint32_t ms_measure(void)
{
    uint64_t start = systimer_read();
    uint64_t mtime_start = counter_read();
    while (systimer_read() - start < SYSTIMER_HZ / 1000u) {
    }
    return (uint32_t)(counter_read() - mtime_start);
}

/* The dividers and the source, then the update that applies them together, as ESP-IDF moves to a PLL. */
static void cpu_clock_set(void)
{
    REG(PCR_CPU_FREQ_CONF) = PCR_CPU_UNDIVIDED;
    REG(PCR_AHB_FREQ_CONF) = PCR_AHB_DIV4;
    REG(PCR_SYSCLK_CONF) = (REG(PCR_SYSCLK_CONF) & ~PCR_SOC_CLK_SEL) | PCR_SOC_CLK_PLL_F160;
    REG(PCR_BUS_CLK_UPDATE) = PCR_BUS_CLOCK_UPDATE;
    while (REG(PCR_BUS_CLK_UPDATE) & PCR_BUS_CLOCK_UPDATE) {
    }
}

/*
 * The SPLL's dividers for the 48 MHz crystal, ESP-IDF's: the reference divided by one and the charge pump 5,
 * the feedback divided by ten, dr1 and dr3 one, the low and high reference selects 1 and 3, and the VCO's bias 3;
 * as read back from the ROM's download mode, which leaves them so. Its calibration runs around the change.
 */
static void sspll_set(void)
{
    REG(PCR_SYSCLK_CONF) = (REG(PCR_SYSCLK_CONF) & ~PCR_SOC_CLK_SEL) | PCR_SOC_CLK_XTAL;
    REG(PCR_AHB_FREQ_CONF) = 0;
    REG(PCR_BUS_CLK_UPDATE) = PCR_BUS_CLOCK_UPDATE;
    while (REG(PCR_BUS_CLK_UPDATE) & PCR_BUS_CLOCK_UPDATE) {
    }
    REG(PMU_IMM_HP_CK_POWER) = PMU_TIE_HIGH_BBPLL;
    REG(MODEM_LPCON_CLK_CONF_FORCE) |= CLK_I2C_MST_FO;
    REG(MODEM_SYSCON_CLK_CONF) |= CLK_I2C_MST_SEL_160M;
    REG(I2C_MST_ANA_CONF2) = (REG(I2C_MST_ANA_CONF2) & I2C_MST_ROUTE_MASK) | I2C_MST_ROUTE;

    REG(I2C_MST_ANA_CONF0) = (REG(I2C_MST_ANA_CONF0) & ~CAL_STOP_HIGH) | CAL_STOP_LOW;
    bbpll_write(2, 0x7f, 0x51);
    bbpll_write(3, 0xff, 0x0a);
    bbpll_write(5, 0x77, 0x11);
    bbpll_write(6, 0xf0, 0x70);
    bbpll_write(9, 0x03, 0x03);
    for (uint32_t n = 0; n < CAL_SPINS && !(REG(I2C_MST_ANA_CONF0) & CAL_DONE); n++) {
    }
    REG(I2C_MST_ANA_CONF0) = (REG(I2C_MST_ANA_CONF0) & ~CAL_STOP_LOW) | CAL_STOP_HIGH;
    REG(MODEM_LPCON_CLK_CONF_FORCE) &= ~CLK_I2C_MST_FO;
}

/*
 * The rate timer_counter_hz returns is measured over a millisecond, after the clock is set,
 * so it is as exact as the loop in ms_measure.
 */
void timer_init(void)
{
    clint_hold();
    REG(CLINT_TIMECTL) = TIMECTL_COUNTER_EN;

    uint32_t found = ms_measure();
    sspll_set();
    cpu_clock_set();
    timer_start(ms_measure() * 1000u);
    kputs("rvuos: cpu clock ");
    kput_hex(timer_counter_hz());
    kputs(" hz, found at ");
    kput_hex(found * 1000u);
    kputs(" hz\n");
}
