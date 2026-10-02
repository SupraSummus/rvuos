/*
 * What mps2-an521 needs before the kernel starts: its devices opened to user mode;
 * and for a kernel built for both cores, how the first starts the second, and how one interrupts the other.
 *
 * The SSE-200 puts every device of the board behind a peripheral protection controller,
 * which refuses an unprivileged access unless the Secure Privilege Control block lets it through,
 * reading zero and dropping a write, as an ESP32-C6's permission units do.
 * The kernel lets user mode through to the devices the root task is granted, UART0 and the FPGA's counter;
 * the MPU confines a thread to its regions there as everywhere, so a device is a process's only through a frame.
 * QEMU leaves no watchdog running.
 */

#include <stdint.h>

#include "armv7m.h"
#include "kernel.h"
#include "layout.h"
#include "object.h"
#include "scs.h"

#define REG(addr) (*(volatile uint32_t *)(addr))

/* The Secure Privilege Control block, and its registers that open the APB expansion ports to unprivileged code. */
#define SPCTRL_BASE   0x50080000u
#define APBSPPPCEXP1  (SPCTRL_BASE + 0xc4u)
#define APBSPPPCEXP2  (SPCTRL_BASE + 0xc8u)

/* Where the board's devices sit behind the expansion controllers. */
#define PORT_UART0  (1u << 5) /* of the second */
#define PORT_FPGAIO (1u << 2) /* of the third */

void board_init(void)
{
    REG(APBSPPPCEXP1) |= PORT_UART0;
    REG(APBSPPPCEXP2) |= PORT_FPGAIO;
}

/*
 * User mode on ARMv8-M writes no state of the core's beyond the registers a trap saves,
 * since the kernel shuts the floating-point unit to it; see kernel/arch/arm/trap.c.
 */
void board_user_csrs_reset(void)
{
}

/*
 * The cores; see DESIGN.md, "Cores".
 * The second core waits in CPUWAIT from reset until the first lets it go,
 * then enters the vector table INITSVTOR1 names, the kernel's own, at its reset vector; see start.S.
 * MHU0 holds a word of status for each core, raised while it is not zero,
 * on line IPI_LINE of that core's NVIC alone, and set and cleared a bit at a time.
 * The line is level, and the NVIC latches it, so a core takes its interrupt back
 * by clearing the status, then the pending state the level left.
 */
#if CORES > 1
_Static_assert(CORES == 2, "the SSE-200 has two cores");

#define INITSVTOR1 (SYSCTL_BASE + 0x114u)
#define CPUWAIT    (SYSCTL_BASE + 0x118u)

/* The words of MHU0 that set bits of core c's status, and that clear them. */
#define MHU_SET(c) (MHU0_BASE + 0x10u * (c) + 0x4u)
#define MHU_CLR(c) (MHU0_BASE + 0x10u * (c) + 0x8u)

#define IPI_WORD (4u * (IPI_LINE / 32))
#define IPI_BIT  (1u << (IPI_LINE % 32))

/* The gates board_init opened are both cores', so the second needs nothing of its own. */
void board_core_init(void)
{
}

void board_cores_start(void)
{
    extern char trap_vectors[];
    REG(INITSVTOR1) = (uint32_t)trap_vectors;
    REG(CPUWAIT) = 0;
}

void ipi_enable(void)
{
    SCS_REG(NVIC_ISER + IPI_WORD) = IPI_BIT;
}

void ipi_send(uint32_t core)
{
    REG(MHU_SET(core)) = 1;
}

void ipi_clear(void)
{
    REG(MHU_CLR(core_index())) = 1;
    SCS_REG(NVIC_ICPR + IPI_WORD) = IPI_BIT;
}
#endif
