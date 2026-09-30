#ifndef RVUOS_ARMV7M_H
#define RVUOS_ARMV7M_H

#include <stdbool.h>
#include <stdint.h>

/*
 * What trap.c needs of systick.c and nvic.c, the timer and the controller every Cortex-M has,
 * and of mpu.c, and what start.S calls.
 */

/*
 * Whether the counter has reached the compare: the timer interrupt is due,
 * as a CLINT's level says on RISC-V, however SysTick's own pending bit stands.
 */
bool counter_due(void);

/* SysTick fired: set it again for what is left before the compare, since one reload may not reach it. */
void counter_tick(void);

/* The line whose exception the trap entered on, which the NVIC no longer shows pending; zero for none. */
extern uint32_t nvic_entered;

/* Whether a line is pending and enabled, which the kernel, running at every line's priority, does not take. */
bool nvic_pending(void);

/* The MPU off as the kernel runs, from the start of a trap, and on as a thread runs, from the end of frame_give. */
void mpu_kernel(void);
void mpu_thread(void);

/* trap.c: the core set up before kmain, and the hardware frame written as a trap returns, where psp is to point. */
void arch_init(void);
struct trap_frame;
uint32_t frame_give(struct trap_frame *f);

#endif
