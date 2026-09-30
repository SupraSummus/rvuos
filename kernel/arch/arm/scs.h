#ifndef RVUOS_SCS_H
#define RVUOS_SCS_H

#include <stdint.h>

/*
 * The System Control Space of ARMv7-M, at the same addresses on every Cortex-M:
 * the system control block, the NVIC, SysTick and the MPU,
 * and what ARMv8-M adds to it, which arch.h's ARMV8M says a core has.
 * Only privileged code reaches it, whatever the MPU says,
 * so no process can be granted any of it.
 */

#define SCS_REG(addr) (*(volatile uint32_t *)(addr))

#define ACTLR 0xe000e008u
#define ACTLR_DISDEFWBUF (1u << 1) /* every bus fault precise, so it is the running thread's; ARMv7-M's */

#define SYST_CSR 0xe000e010u
#define SYST_RVR 0xe000e014u
#define SYST_CVR 0xe000e018u
#define SYST_CSR_ENABLE    (1u << 0)
#define SYST_CSR_TICKINT   (1u << 1)
#define SYST_CSR_CLKSOURCE (1u << 2) /* the processor's clock */
#define SYST_MAX 0x01000000u         /* the counts one reload of the 24-bit counter lasts at most */

#define NVIC_ISER 0xe000e100u /* one bit per line: enable, and read back */
#define NVIC_ICER 0xe000e180u
#define NVIC_ISPR 0xe000e200u
#define NVIC_ICPR 0xe000e280u
#define NVIC_IPR  0xe000e400u /* one byte per line: its priority */

#define ICSR 0xe000ed04u
#define ICSR_PENDSTCLR (1u << 25)
#define ICSR_PENDSTSET (1u << 26)
#define ICSR_PENDSVSET (1u << 28)
#define VTOR  0xe000ed08u
#define SCR   0xe000ed10u
#define SCR_SEVONPEND (1u << 4) /* a line becoming pending wakes wfe, taken or not */
#define CCR   0xe000ed14u
#define CCR_STKALIGN (1u << 9) /* the hardware frame lies on eight bytes, and xpsr bit 9 says it moved */
#define SHPR1 0xe000ed18u
#define SHPR2 0xe000ed1cu
#define SHPR3 0xe000ed20u
#define SHCSR 0xe000ed24u
#define SHCSR_MEMFAULTPENDED (1u << 13)
#define SHCSR_BUSFAULTPENDED (1u << 14)
#define SHCSR_SVCALLPENDED (1u << 15)
#define SHCSR_MEMFAULTENA  (1u << 16)
#define SHCSR_BUSFAULTENA  (1u << 17)
#define SHCSR_USGFAULTENA  (1u << 18)
#define SHCSR_SECUREFAULTENA (1u << 19) /* ARMv8-M's */
#define CFSR  0xe000ed28u
#define CFSR_MUNSTKERR (1u << 3)
#define CFSR_MSTKERR   (1u << 4)
#define CFSR_MMARVALID (1u << 7)
#define CFSR_UNSTKERR  (1u << 11)
#define CFSR_STKERR    (1u << 12)
#define CFSR_BFARVALID (1u << 15)
#define HFSR  0xe000ed2cu
#define MMFAR 0xe000ed34u
#define BFAR  0xe000ed38u
#define CPACR 0xe000ed88u /* two bits per coprocessor: who may use it */
#define NSACR 0xe000ed8cu /* ARMv8-M's: which coprocessors the Non-secure state may use */
#define DEMCR 0xe000edfcu
#define DEMCR_MON_EN (1u << 16) /* a bkpt is a DebugMonitor exception, not a HardFault */

#define MPU_TYPE 0xe000ed90u
#define MPU_CTRL 0xe000ed94u
#define MPU_RNR  0xe000ed98u
#define MPU_RBAR 0xe000ed9cu
#define MPU_RASR 0xe000eda0u  /* PMSAv7's size and attributes */
#define MPU_RLAR 0xe000eda0u  /* PMSAv8's limit and attributes, where PMSAv7 has RASR */
#define MPU_MAIR0 0xe000edc0u /* PMSAv8's memory attributes, a byte for each index a region names */
#define MPU_CTRL_ENABLE     (1u << 0)
#define MPU_CTRL_PRIVDEFENA (1u << 2) /* the kernel sees the default map wherever no region lies */

/* ARMv8-M's Security Extension: the attribution unit, and what a SecureFault left. */
#define SAU_CTRL 0xe000edd0u
#define SFSR     0xe000ede4u
#define SFAR     0xe000ede8u
#define SFSR_SFARVALID (1u << 6)

/* Exception numbers, as IPSR holds them; line n of the NVIC is 16 + n. */
#define EXC_NMI        2
#define EXC_HARDFAULT  3
#define EXC_MEMMANAGE  4
#define EXC_BUSFAULT   5
#define EXC_USAGEFAULT 6
#define EXC_SECUREFAULT 7 /* ARMv8-M's, with the Security Extension */
#define EXC_SVCALL     11
#define EXC_DEBUGMON   12
#define EXC_PENDSV     14
#define EXC_SYSTICK    15
#define EXC_IRQ0       16

/* The thread-mode return on the process stack, as the core leaves it in lr for a trap from user mode. */
#define EXC_RETURN_THREAD_PSP 0xfffffffdu

#define XPSR_T (1u << 24)
/* N, Z, C, V and Q, which a branch keeps; the IT state it leaves lies in bits 26:25 and 15:10. */
#define XPSR_APSR (0x1fu << 27)
#define XPSR_ALIGNED (1u << 9)

#endif
