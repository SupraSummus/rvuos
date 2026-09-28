#ifndef RVUOS_CLINT_H
#define RVUOS_CLINT_H

#include <stdint.h>

/*
 * The machine timer as a CLINT has it:
 * mtime, a 64-bit counter, and mtimecmp, whose interrupt is a level while mtime has reached it.
 * clint.c drives it for timer.h the same way on every board,
 * at the addresses the board's board.h names, CLINT_MTIME and CLINT_MTIMECMP.
 * What starts the counter, and how fast it counts, is the board's timer.c,
 * whose timer_init ends in clint_start; see DESIGN.md, "Timer".
 */

/* The counter's value. */
uint64_t clint_mtime(void);

/* Push mtimecmp out of reach, so that the timer's level stays low until clint_start. */
void clint_hold(void);

/*
 * Tick every period counts of mtime, the first a period from now,
 * and enable the machine timer interrupt;
 * timer_counter_hz is period times TIMER_HZ from then on.
 */
void clint_start(uint32_t period);

#endif
