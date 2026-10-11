#ifndef RVUOS_THREAD_ALARM_H
#define RVUOS_THREAD_ALARM_H

/*
 * Alarms in microseconds, finer than the kernel's tick, on a TIMER of the nRF52840's that the kernel leaves free,
 * as a driver in user mode drives it: alarm.c.
 * The TIMER counts microseconds, 32 bits, from when alarm_init starts it, and wraps every 71 minutes;
 * each of its compare registers but the last is an alarm, which raises the TIMER's line once its count comes.
 * The driver arms the line's Irq and waits on its notification as on the radio's.
 * Nothing here keeps a global: the TIMER's state is handed in.
 */

#include <stdbool.h>
#include <stdint.h>

/* The alarms, a TIMER's compare registers but the last, which reads its count. */
#define ALARMS 3u

struct alarm {
    uint32_t base; /* the registers, mapped at their own address */
};

/* Starts the TIMER counting microseconds, every alarm off. */
void alarm_init(struct alarm *a, uint32_t base);

/* The TIMER's count, in microseconds. */
uint32_t alarm_now(struct alarm *a);

/*
 * Sets alarm n, below ALARMS, for the count at: true if it will fire,
 * false if at has come already, and the alarm is off.
 */
bool alarm_set(struct alarm *a, uint32_t n, uint32_t at);

/* Turns alarm n off; it fires no more until set again. */
void alarm_off(struct alarm *a, uint32_t n);

/* After the line: whether alarm n fired, which turns it off. */
bool alarm_fired(struct alarm *a, uint32_t n);

#endif
