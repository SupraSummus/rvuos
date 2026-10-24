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
 * The SPLL needs no start: the USB Serial/JTAG controller, the console's and the loader's, runs on it.
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
_Static_assert(CLINT_MTIME == COUNTER_ADDR, "the clock names the counter the timer compares with");
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

#define REG(addr) (*(volatile uint32_t *)(addr))

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
 * The rate timer_counter_hz returns is measured over a millisecond, after the clock is set,
 * so it is as exact as the loop in ms_measure.
 */
void timer_init(void)
{
    clint_hold();
    REG(CLINT_TIMECTL) = TIMECTL_COUNTER_EN;

    uint32_t found = ms_measure();
    cpu_clock_set();
    timer_start(ms_measure() * 1000u);
    kputs("rvuos: cpu clock ");
    kput_hex(timer_counter_hz());
    kputs(" hz, found at ");
    kput_hex(found * 1000u);
    kputs(" hz\n");
}
