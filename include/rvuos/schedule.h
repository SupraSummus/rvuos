#ifndef RVUOS_SCHEDULE_H
#define RVUOS_SCHEDULE_H

/*
 * The calls of a kernel built with SCHED=user, which rvuos/abi.h includes then.
 * They live apart from it so that tools/seed.py, the fuzzer and the replay, which read abi.h alone, never meet them.
 */

/*
 * An experiment's, built with SCHED=user: a scheduler thread chooses which account's turn comes next.
 * An account is a place with units, named by its first unit; a ready thread runs on it, the place's own or its borrower's.
 * Both calls take a Time capability with RIGHT_W over every unit of the caller's core,
 * and fail with KERR_STATE under tracing, which takes turns in order alone.
 * Times are counts of the core's counter, modulo 2^32, as the ticks count them: a tick's counts for each tick and the offset into it.
 *
 * Time (RIGHT_W): choose the next turn, and wait for the next choice.
 * The caller becomes its core's scheduler, unless another thread is, which fails with KERR_OVERLAP.
 * a1 = the first unit of the account whose turn it is, a bit of the ready mask, or SCHED_NONE for none, which leaves spare time to run;
 * a2 = the counts the turn may run before the scheduler chooses again, at most to the next tick, or 0 for until it waits.
 * Returns once the core must choose again: the turn ended, by a wait, a tick or the limit, or a thread became ready while it ran.
 * a1, a2 = the ready mask, bit i for the account whose first unit is the core's i-th: those with a ready thread, the turn's among them;
 * a3 = the virtual time of the account the last turn chosen ran on, the count at which it would be full, 0 if none was chosen;
 * a4 = the count.
 * Fails with KERR_STATE, the mask in a1 and a2, if a1 names no account with a ready thread.
 * The scheduler runs first whenever its core must choose, on time nobody pays for, and waits on no queue.
 */
#define OP_TIME_SCHEDULE 47
#define SCHED_NONE 0xffffffffu
/*
 * Time (RIGHT_W): an account's virtual time, as OP_TIME_SCHEDULE tells it.
 * a1 = its first unit. Returns a1 = its virtual time, a2 = its units, a3 = the count.
 * Fails with KERR_INVALID_ARG if a1 is not the first unit of an account of the core.
 */
#define OP_TIME_DUE 48

#endif
