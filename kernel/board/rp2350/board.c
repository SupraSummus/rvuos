/*
 * What RP2350 needs before the kernel starts.
 *
 * The kernel sets the clocks rather than trust the ones the bootrom left:
 * clk_ref from the 12 MHz crystal, clk_sys at 150 MHz from PLL_SYS,
 * and clk_usb at 48 MHz from PLL_USB for the halt, see halt.c;
 * the USB controller needs clk_sys above clk_usb, erratum RP2350-E12.
 * The tick generators divide clk_ref into microseconds
 * for Hazard3's mtime, for TIMER0's counter behind BOOT_CAP_CLOCK, and for the watchdog,
 * which is armed at once, and fed only with the kernel's; see bootsel.h and board_watchdog.
 * Register addresses are those of the pico-sdk's hardware_regs for RP2350.
 *
 * User mode reaches a peripheral only where ACCESSCTRL lets it in, and at reset it lets it into few.
 * TIMER0 is opened for the clock.
 * A process reaches it only through a frame besides, on either kind of core:
 * on Hazard3 the fence shuts the hardwired PMP entries that would leave every peripheral to user mode,
 * see process_fence, and on the Cortex-M33 a thread reaches only what its regions hold.
 * Hazard3's counters are shut to user mode, as on QEMU, so rdtime and rdcycle trap.
 */

#include <stdint.h>

#include "arch.h"
#include "bootsel.h"
#include "kernel.h"
#include "layout.h"
#include "object.h"
#include "timer.h"
#ifdef __riscv
#include "csr.h"
#else
#include "scs.h"
#endif

#define REG(addr) (*(volatile uint32_t *)(addr))
/* The atomic aliases every peripheral register has. */
#define SET(addr) REG((addr) + 0x2000u)
#define CLR(addr) REG((addr) + 0x3000u)

#define RESETS_RESET      0x40020000u
#define RESETS_RESET_DONE 0x40020008u
#define RESET_IO_BANK0    (1u << 6)
#define RESET_PADS_BANK0  (1u << 9)
#define RESET_PIO0        (1u << 11)
#define RESET_PIO1        (1u << 12)
#define RESET_PIO2        (1u << 13)
#define RESET_PLL_SYS     (1u << 14)
#define RESET_PLL_USB     (1u << 15)
#define RESET_TIMER0      (1u << 23)
/* The devices of board.h's DEVICE_RANGE_LIST. */
#define RESET_DEVICE_RANGES (RESET_IO_BANK0 | RESET_PADS_BANK0 | RESET_PIO0 | RESET_PIO1 | RESET_PIO2)

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

#define ACCESSCTRL_GPIO_NSMASK0 0x4006000cu
#define ACCESSCTRL_GPIO_NSMASK1 0x40060010u
#define ACCESSCTRL_PIO0     0x4006004cu
#define ACCESSCTRL_PIO1     0x40060050u
#define ACCESSCTRL_PIO2     0x40060054u
#define ACCESSCTRL_IO_BANK0 0x40060068u
#define ACCESSCTRL_PADS_BANK0 0x40060070u
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

/* Lets user mode into a device, keeping who else may reach it. */
static void open_to_user(uint32_t reg)
{
    uint32_t was = REG(reg) & 0xffu;
    REG(reg) = ACCESSCTRL_PASSWORD | was | ACCESSCTRL_USER;
}

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
    open_to_user(ACCESSCTRL_TIMER0);

    /*
     * The devices the root task is granted, see board.h, out of reset and open to user mode.
     * Hazard3's user mode is Non-secure on the bus, and IO_BANK0 and PADS_BANK0 show it only the pins
     * the masks let it reach, which are all of them; the kernel drives none.
     */
    unreset(RESET_DEVICE_RANGES);
    open_to_user(ACCESSCTRL_IO_BANK0);
    open_to_user(ACCESSCTRL_PADS_BANK0);
    open_to_user(ACCESSCTRL_PIO0);
    open_to_user(ACCESSCTRL_PIO1);
    open_to_user(ACCESSCTRL_PIO2);
#ifdef __riscv
    REG(ACCESSCTRL_GPIO_NSMASK0) = 0xffffffffu;
    REG(ACCESSCTRL_GPIO_NSMASK1) = 0x0000ffffu;
#endif

    /* The console starts empty, whatever the last image left in it; see halt.c. */
    REG(UART_BASE) = 0;
}

/*
 * The chip's watchdog reboots into BOOTSEL a second after the kernel's watchdog halts the machine,
 * so only a kernel that stopped altogether, and cannot halt, meets it.
 */
#define WATCHDOG_MARGIN_US 1000000u
_Static_assert(WATCHDOG_US_MAX + WATCHDOG_MARGIN_US <= WATCHDOG_LONGEST, "the chip's watchdog counts as far");

void board_watchdog(uint32_t us)
{
    bootsel_feed(us + WATCHDOG_MARGIN_US);
}

/*
 * Hazard3 has no CSR user mode writes, and the counters stay shut.
 * The Cortex-M33's thread mode keeps no state beyond the registers a trap saves,
 * since the kernel shuts every coprocessor to it, the floating-point unit among them; see kernel/arch/arm/trap.c.
 */
void board_user_csrs_reset(void)
{
}

/*
 * The cores; see DESIGN.md, "Cores".
 * The second waits in the bootrom from reset until the first launches it through SIO's FIFOs,
 * each core writing the other's and reading its own:
 * it takes 0, 0, 1, its vector table, its stack and its entry a word at a time, answering each with the word it took,
 * and a word out of turn makes it answer and start again from the first, which a 0 always is.
 * So the first empties what it has to read before each 0, and begins again on a wrong answer,
 * as the datasheet's "Launching code on processor core 1" has it.
 * On Hazard3 the vector table is the value for mtvec.
 * The second core then enters the kernel at _start, which tells it from the first by its number, see start.S.
 */
#if CORES > 1
_Static_assert(CORES == 2, "RP2350 has two cores");

#define SIO_FIFO_ST (SIO_BASE + 0x50u)
#define SIO_FIFO_WR (SIO_BASE + 0x54u)
#define SIO_FIFO_RD (SIO_BASE + 0x58u)
#define FIFO_VLD    (1u << 0) /* a word from the other core to read */
#define FIFO_RDY    (1u << 1) /* room for a word to it */
#define FIFO_DRAIN  8u        /* words read before a 0; one left over only begins the launch again */

/* How long the second core has for each word, in the counter's microseconds, and how many words the launch may take. */
#define LAUNCH_WORD_US  10000u
#define LAUNCH_WORDS    6u
#define LAUNCH_ATTEMPTS (4u * LAUNCH_WORDS)

_Static_assert(COUNTER_HZ == 1000000u, "the launch counts microseconds");

/* The event the bootrom's second core may wait for, as the pico-sdk's launch sends it: sev, or Hazard3's h3.unblock. */
static void core1_wake(void)
{
#ifdef __riscv
    __asm__ volatile("slt x0, x0, x1" : : : "memory"); /* h3.unblock */
#else
    __asm__ volatile("dsb\n\tsev" : : : "memory");
#endif
}

/* Whether the FIFOs' status shows what within the time the second core has for a word. */
static bool fifo_shows(uint32_t what)
{
    uint64_t until = counter_read() + LAUNCH_WORD_US;
    while ((REG(SIO_FIFO_ST) & what) == 0) {
        LOOP_WAIT("the second core to take or answer a word, or its time for it to pass");
        if (counter_read() >= until) {
            return false;
        }
    }
    return true;
}

void board_cores_start(void)
{
    extern char trap_vectors[];
    extern char __kernel_stack_top[];
    extern void _start(void);
    const uint32_t launch[LAUNCH_WORDS] = {
        0,
        0,
        1,
#ifdef __riscv
        (uint32_t)trap_vectors | 1u, /* vectored, as start.S sets it again */
#else
        (uint32_t)trap_vectors,
#endif
        (uint32_t)__kernel_stack_top - KERNEL_STACK_SIZE,
        ENTRY_PC((uint32_t)&_start),
    };
    uint32_t taken = 0;
    for (uint32_t attempt = 0; attempt < LAUNCH_ATTEMPTS && taken < LAUNCH_WORDS; attempt++) {
        LOOP_BOUND(LAUNCH_ATTEMPTS);
        uint32_t word = launch[taken];
        if (word == 0) {
            for (uint32_t i = 0; i < FIFO_DRAIN && (REG(SIO_FIFO_ST) & FIFO_VLD) != 0; i++) {
                LOOP_BOUND(FIFO_DRAIN);
                (void)REG(SIO_FIFO_RD);
            }
            core1_wake();
        }
        if (!fifo_shows(FIFO_RDY)) {
            break;
        }
        REG(SIO_FIFO_WR) = word;
        core1_wake();
        if (!fifo_shows(FIFO_VLD)) {
            break;
        }
        taken = REG(SIO_FIFO_RD) == word ? taken + 1 : 0;
    }
    if (taken < LAUNCH_WORDS) {
        kpanic("the second core did not take its launch");
    }
#ifdef __riscv
    /* start.S has Hazard3's second core wait for its software interrupt, as QEMU virt's harts do. */
    ipi_send(1);
#endif
}

/* What board_init does on the first core that the second needs of its own: Hazard3's counters are shut there too. */
void board_core_init(void)
{
#ifdef __riscv
    csr_write(mcounteren, 0);
#endif
}

#ifdef __riscv
/*
 * Hazard3's software interrupt, mip's MSIP, is a flag of SIO's for each core,
 * which RISCV_SOFTIRQ sets by bit c and clears by bit 8 + c, for core c, from either core.
 */
#define SIO_RISCV_SOFTIRQ (SIO_BASE + 0x1a0u)
#define SOFTIRQ_SET(c)    (1u << (c))
#define SOFTIRQ_CLR(c)    (1u << (8u + (c)))

void ipi_enable(void)
{
    csr_set(mie, MIE_MSIE);
}

void ipi_send(uint32_t core)
{
    REG(SIO_RISCV_SOFTIRQ) = SOFTIRQ_SET(core);
}

void ipi_clear(void)
{
    REG(SIO_RISCV_SOFTIRQ) = SOFTIRQ_CLR(core_index());
}
#else
/*
 * The Cortex-M33's doorbells: a bit written to DOORBELL_OUT_SET sets that bit of the other core's DOORBELL_IN,
 * which raises SIO_IRQ_BELL there, IPI_LINE, while any bit of it is set, and DOORBELL_IN_CLR clears them.
 * The line is level, and the NVIC latches it, so a core takes its interrupt back
 * by clearing its doorbell, then the pending state the level left, as mps2-an521's board.c does.
 */
#define SIO_DOORBELL_OUT_SET (SIO_BASE + 0x180u)
#define SIO_DOORBELL_IN_CLR  (SIO_BASE + 0x18cu)

#define IPI_WORD (4u * (IPI_LINE / 32))
#define IPI_BIT  (1u << (IPI_LINE % 32))

void ipi_enable(void)
{
    SCS_REG(NVIC_ISER + IPI_WORD) = IPI_BIT;
}

/* The doorbell rings on the other core, which is the one of two cores a core ever interrupts. */
void ipi_send(uint32_t core)
{
    (void)core;
    REG(SIO_DOORBELL_OUT_SET) = 1u;
}

void ipi_clear(void)
{
    REG(SIO_DOORBELL_IN_CLR) = 1u;
    SCS_REG(NVIC_ICPR + IPI_WORD) = IPI_BIT;
}
#endif
#endif
