#ifndef RVUOS_CLINT_H
#define RVUOS_CLINT_H

/*
 * The machine timer as a CLINT has it:
 * mtime, a 64-bit counter, and mtimecmp, whose interrupt is a level while mtime has reached it.
 * clint.c makes them the counter and compare of timer.h the same way on every RISC-V board,
 * at the addresses the board's board.h names, CLINT_MTIME and CLINT_MTIMECMP.
 * What starts the counter, and how fast it counts, is the board's timer.c,
 * whose timer_init ends in timer_start; see DESIGN.md, "Timer".
 */

/* Push mtimecmp out of reach, so that the timer's level stays low until timer_start. */
void clint_hold(void);

#endif
