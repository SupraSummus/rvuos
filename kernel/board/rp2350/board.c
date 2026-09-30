/*
 * What RP2350 needs before the kernel starts.
 *
 * The kernel sets the clocks rather than trust the ones the bootrom left:
 * clk_ref from the 12 MHz crystal, clk_sys at 150 MHz from PLL_SYS,
 * and clk_usb at 48 MHz from PLL_USB for the halt, see halt.c;
 * the USB controller needs clk_sys above clk_usb, erratum RP2350-E12.
 * The tick generators divide clk_ref into microseconds
 * for Hazard3's mtime, for TIMER0's counter behind BOOT_CAP_CLOCK, and for the watchdog,
 * which is armed at once; see bootsel.h.
 * Register addresses are those of the pico-sdk's hardware_regs for RP2350.
 *
 * User mode reaches a peripheral only where ACCESSCTRL lets it in, and at reset it lets it into few.
 * TIMER0 is opened for the clock.
 * On Hazard3 the hardwired PMP entries leave every peripheral to user mode,
 * so it is open to every process; see DESIGN.md, "Boards".
 * On the Cortex-M33 a thread reaches only what its regions hold, a peripheral as well as RAM.
 * Hazard3's counters are shut to user mode, as on QEMU, so rdtime and rdcycle trap.
 */

#include <stdint.h>

#include "bootsel.h"
#include "kernel.h"
#include "layout.h"
#ifdef __riscv
#include "csr.h"
#endif

#define REG(addr) (*(volatile uint32_t *)(addr))
/* The atomic aliases every peripheral register has. */
#define SET(addr) REG((addr) + 0x2000u)
#define CLR(addr) REG((addr) + 0x3000u)

#define RESETS_RESET      0x40020000u
#define RESETS_RESET_DONE 0x40020008u
#define RESET_PLL_SYS     (1u << 14)
#define RESET_PLL_USB     (1u << 15)
#define RESET_TIMER0      (1u << 23)

#define XOSC_CTRL     0x40048000u
#define XOSC_STATUS   0x40048004u
#define XOSC_STARTUP  0x4004800cu
#define XOSC_ENABLE   (0xfabu << 12)
#define XOSC_1_15MHZ  0xaa0u
#define XOSC_STABLE   (1u << 31)
/*
 * Six milliseconds of the crystal, in units of 256 of its cycles, as the pico-sdk waits for slow-starting ones:
 * STABLE says only that the count ran out.
 */
#define XOSC_DELAY    (6u * 47u)

#define PLL_SYS_BASE  0x40050000u
#define PLL_USB_BASE  0x40058000u
#define PLL_CS        0x0u
#define PLL_PWR       0x4u
#define PLL_FBDIV_INT 0x8u
#define PLL_PRIM      0xcu
#define PLL_LOCK      (1u << 31)
#define PLL_PD        (1u << 0)
#define PLL_POSTDIVPD (1u << 3)
#define PLL_VCOPD     (1u << 5)

#define CLK_REF_CTRL     0x40010030u
#define CLK_REF_DIV      0x40010034u
#define CLK_REF_SELECTED 0x40010038u
#define CLK_SYS_CTRL     0x4001003cu
#define CLK_SYS_DIV      0x40010040u
#define CLK_SYS_SELECTED 0x40010044u
#define CLK_USB_CTRL     0x40010060u
#define CLK_USB_DIV      0x40010064u
#define CLK_REF_SRC_XOSC 2u
#define CLK_SYS_SRC_AUX  1u         /* SRC: the auxiliary mux, whose AUXSRC 0 is PLL_SYS */
#define CLK_ENABLE       (1u << 11)
#define CLK_DIV_ONE      (1u << 16) /* an integer divider of one */

/* A tick generator's control and cycle count; the ones for TIMER0, the watchdog and mtime. */
#define TICKS_CTRL(gen)   (0x40108000u + (gen))
#define TICKS_CYCLES(gen) (0x40108004u + (gen))
#define TICKS_TIMER0      0x18u
#define TICKS_WATCHDOG    0x30u
#define TICKS_RISCV       0x3cu
#define TICKS_ENABLE      1u
#define TICKS_PER_US      12u /* of clk_ref, the crystal's */

#define ACCESSCTRL_TIMER0   0x40060098u
#define ACCESSCTRL_PASSWORD 0xacce0000u
#define ACCESSCTRL_SU       (1u << 2)
#define ACCESSCTRL_NSP      (1u << 1)
#define ACCESSCTRL_NSU      (1u << 0)

/*
 * The bits of ACCESSCTRL that let user mode in:
 * Hazard3's user mode reaches the bus as Non-secure,
 * and the Cortex-M33's unprivileged thread mode as Secure, the state the kernel keeps it in.
 */
#ifdef __riscv
#define ACCESSCTRL_USER (ACCESSCTRL_NSP | ACCESSCTRL_NSU)
#else
#define ACCESSCTRL_USER ACCESSCTRL_SU
#endif

static void unreset(uint32_t blocks)
{
    SET(RESETS_RESET) = blocks;
    CLR(RESETS_RESET) = blocks;
    while ((REG(RESETS_RESET_DONE) & blocks) != blocks) {
    }
}

/* A PLL at 12 MHz times fbdiv, divided by both post dividers; the datasheet's recipe. */
static void pll_start(uint32_t base, uint32_t fbdiv, uint32_t postdiv1, uint32_t postdiv2)
{
    REG(base + PLL_CS) = 1; /* the reference undivided */
    REG(base + PLL_FBDIV_INT) = fbdiv;
    CLR(base + PLL_PWR) = PLL_PD | PLL_VCOPD;
    while ((REG(base + PLL_CS) & PLL_LOCK) == 0) {
    }
    REG(base + PLL_PRIM) = (postdiv1 << 16) | (postdiv2 << 12);
    CLR(base + PLL_PWR) = PLL_POSTDIVPD;
}

static void tick_start(uint32_t gen)
{
    REG(TICKS_CTRL(gen)) = 0;
    REG(TICKS_CYCLES(gen)) = TICKS_PER_US;
    REG(TICKS_CTRL(gen)) = TICKS_ENABLE;
}

static void clocks_init(void)
{
    /* The crystal may be running already, and its range is not changed under it. */
    if ((REG(XOSC_STATUS) & XOSC_STABLE) == 0) {
        REG(XOSC_CTRL) = XOSC_1_15MHZ;
        REG(XOSC_STARTUP) = XOSC_DELAY;
        SET(XOSC_CTRL) = XOSC_ENABLE;
        while ((REG(XOSC_STATUS) & XOSC_STABLE) == 0) {
        }
    }

    /*
     * clk_sys leaves its auxiliary mux for clk_ref while the PLL under it changes.
     * clk_ref leaves the ROSC before its divider drops to one:
     * the bootrom runs the ROSC four times faster and at a random frequency, with a divider of four behind it,
     * and dropping the divider first ran clk_ref and clk_sys that fast for a moment, which now and then hung the chip.
     */
    REG(CLK_SYS_CTRL) = 0;
    while (REG(CLK_SYS_SELECTED) != 1u) {
    }
    REG(CLK_REF_CTRL) = CLK_REF_SRC_XOSC;
    while (REG(CLK_REF_SELECTED) != 1u << CLK_REF_SRC_XOSC) {
    }
    REG(CLK_REF_DIV) = CLK_DIV_ONE;

    unreset(RESET_PLL_SYS | RESET_PLL_USB);
    pll_start(PLL_SYS_BASE, 125, 5, 2); /* 1500 MHz / 10 = 150 MHz */
    pll_start(PLL_USB_BASE, 100, 5, 5); /* 1200 MHz / 25 = 48 MHz */

    REG(CLK_SYS_DIV) = CLK_DIV_ONE;
    REG(CLK_SYS_CTRL) = CLK_SYS_SRC_AUX;
    while (REG(CLK_SYS_SELECTED) != 1u << CLK_SYS_SRC_AUX) {
    }
    REG(CLK_USB_DIV) = CLK_DIV_ONE;
    REG(CLK_USB_CTRL) = CLK_ENABLE;

    tick_start(TICKS_TIMER0);
    tick_start(TICKS_WATCHDOG);
#ifdef __riscv
    tick_start(TICKS_RISCV);
#endif
}

void board_init(void)
{
#ifdef __riscv
    csr_write(mcounteren, 0);
#endif
    clocks_init();
    bootsel_arm(WATCHDOG_LONGEST);

    unreset(RESET_TIMER0);
    uint32_t timer0 = REG(ACCESSCTRL_TIMER0) & 0xffu;
    REG(ACCESSCTRL_TIMER0) = ACCESSCTRL_PASSWORD | timer0 | ACCESSCTRL_USER;

    /* The console starts empty, whatever the last image left in it; see halt.c. */
    REG(UART_BASE) = 0;
}

/*
 * Hazard3 has no CSR user mode writes, and the counters stay shut.
 * The Cortex-M33's thread mode keeps no state beyond the registers a trap saves,
 * since the kernel shuts every coprocessor to it, the floating-point unit among them; see kernel/arch/arm/trap.c.
 */
void board_user_csrs_reset(void)
{
}
