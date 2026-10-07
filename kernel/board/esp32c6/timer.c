/*
 * Machine timer of the ESP32-C6: the CLINT's mtime and mtimecmp, at 0x20001800.
 *
 * The CLINT here is Espressif's: a control word next to the counter
 * starts it and lets its compare raise the machine timer interrupt,
 * which the ROM leaves off, and then it behaves as a standard one,
 * a level while mtime has reached mtimecmp, on mcause 7, and clint.c drives it.
 * It counts at the CPU clock, so the kernel measures it at boot against the system timer,
 * which runs at 16 MHz off the crystal whatever the CPU clock is.
 *
 * The kernel sets the CPU clock here, before measuring it, rather than keep the one it finds:
 * the ROM sets none, and the reset into its download mode keeps PCR's, as measured on the chip,
 * so it is whatever the flash's last program left, 80 MHz after an ESP-IDF bootloader with no application.
 * 160 MHz is the SPLL's 480 divided by three, and the AHB and APB a quarter of that, as the bootloader leaves them.
 * The SPLL needs no start: the USB Serial/JTAG controller, the console's and the loader's, runs on it,
 * as ESP-IDF's rtc_clk.c counts it among the PLL's users.
 * The boot's log says the clock set and the one found, both measured.
 */

#include <stdint.h>

#include "clint.h"
#include "kernel.h"
#include "layout.h"
#include "timer.h"

#define CLINT_BASE     0x20001800u
#define CLINT_TIMECTL  (CLINT_BASE + 0x04u)
_Static_assert(CLINT_MTIME == CLINT_BASE + 0x08u && CLINT_MTIMECMP == CLINT_BASE + 0x10u,
               "board.h names this CLINT's registers");
/* The copy of mtime user mode reads, 0x400 bytes up among the user timer's registers. */
#define CLINT_UTIME    (CLINT_BASE + 0x408u)
_Static_assert(CLINT_UTIME == COUNTER_ADDR, "the clock names the counter the tick is made of");
#define TIMECTL_COUNTER_EN  (1u << 0)
#define TIMECTL_TIMERINT_EN (1u << 1)

/* The system timer's unit 0, which counts from reset. */
#define SYSTIMER_BASE     0x6000A000u
#define SYSTIMER_UNIT0_OP (SYSTIMER_BASE + 0x04u)
#define SYSTIMER_UNIT0_HI (SYSTIMER_BASE + 0x40u)
#define SYSTIMER_UNIT0_LO (SYSTIMER_BASE + 0x44u)
#define SYSTIMER_UPDATE   (1u << 30) /* latch the count into HI and LO */
#define SYSTIMER_VALID    (1u << 29) /* the latched count is there to read */
#define SYSTIMER_HZ       16000000u

/* The CPU's clock, in PCR: its source, and the dividers below the HP root for the CPU, the AHB and the APB. */
#define PCR_SYSCLK_CONF     0x60096110u
#define PCR_SOC_CLK_SEL     (3u << 16)
#define PCR_SOC_CLK_SPLL    (1u << 16)
#define PCR_CPU_FREQ_CONF   0x60096118u
#define PCR_CPU_UNDIVIDED   0u         /* CPU_HS_DIV_NUM 0, and not CPU_HS_120M_FORCE */
#define PCR_AHB_FREQ_CONF   0x6009611cu
#define PCR_AHB_HS_DIV4     (3u << 8)  /* AHB_HS_DIV_NUM 3 */
#define PCR_APB_FREQ_CONF   0x60096120u
#define PCR_APB_UNDIVIDED   0u

#define REG(addr) (*(volatile uint32_t *)(addr))

static uint64_t systimer_read(void)
{
    REG(SYSTIMER_UNIT0_OP) = SYSTIMER_UPDATE;
    while ((REG(SYSTIMER_UNIT0_OP) & SYSTIMER_VALID) == 0) {
    }
    return ((uint64_t)REG(SYSTIMER_UNIT0_HI) << 32) | REG(SYSTIMER_UNIT0_LO);
}

/* One tick of the system timer, counted in mtime: the tick's length at the CPU clock. */
static uint32_t tick_measure(void)
{
    uint64_t start = systimer_read();
    uint64_t mtime_start = counter_read();
    while (systimer_read() - start < SYSTIMER_HZ / TIMER_HZ) {
    }
    return (uint32_t)(counter_read() - mtime_start);
}

/* The dividers, then the source, as ESP-IDF moves to the SPLL. */
static void cpu_clock_set(void)
{
    REG(PCR_CPU_FREQ_CONF) = PCR_CPU_UNDIVIDED;
    REG(PCR_AHB_FREQ_CONF) = PCR_AHB_HS_DIV4;
    REG(PCR_APB_FREQ_CONF) = PCR_APB_UNDIVIDED;
    REG(PCR_SYSCLK_CONF) = (REG(PCR_SYSCLK_CONF) & ~PCR_SOC_CLK_SEL) | PCR_SOC_CLK_SPLL;
}

/*
 * The rate timer_counter_hz returns is measured over one tick, after the clock is set,
 * so it is as exact as the loop in tick_measure.
 */
void timer_init(void)
{
    clint_hold();
    REG(CLINT_TIMECTL) = TIMECTL_COUNTER_EN | TIMECTL_TIMERINT_EN;

    uint32_t found = tick_measure();
    cpu_clock_set();
    timer_start(tick_measure());
    kputs("rvuos: cpu clock ");
    kput_hex(timer_counter_hz());
    kputs(" hz, found at ");
    kput_hex(found * TIMER_HZ);
    kputs(" hz\n");
}
