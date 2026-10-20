#ifndef RVUOS_WIFI_OSI_H
#define RVUOS_WIFI_OSI_H

/* The driver's pieces that meet in the adapter, and that reach each other. */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "drv.h"
#include "esp.h"
#include "lib/lock.h"
#include "lib/self.h"

struct thread;

/*
 * osi.c: the adapter, the threads it runs everything on,
 * built from the driver's own account s, its timers, which its timer thread runs,
 * and the chip's random number generator.
 * A thread that did not start as one of the adapter's, the driver's first, joins them with osi_adopt,
 * its stack between stack_lo and stack_hi, before it runs.
 * osi_stack_used says the deepest a thread osi_thread made has gone into its stack.
 * The driver's own code waits on the adapter's queues and mutexes by these:
 * a wait of OSI_FOREVER milliseconds ends only when it is met, and one of 0 does not wait;
 * a put or a get returns 1 if it was done.
 *
 * The driver's own code takes no memory as it runs: each of these objects, and each thread's stack,
 * lies where the code that uses it declares it, and a zeroed one is ready, a mutex free and an event group clear,
 * but for a timer, which names its function, and a queue, which OSI_QUEUE declares with its slots.
 */
#define OSI_FOREVER 0xffffffffu

struct waiters {
    struct thread *first;
};

struct mutex {
    struct waiters w;
    struct thread *owner;
    uint32_t depth;
    int recursive;
};

struct queue {
    struct waiters senders, receivers;
    uint32_t len, item_size, head, count;
    uint8_t *items;
};

struct event_group {
    struct waiters w;
    uint32_t bits;
};

/* A queue of n items of size bytes each, name, and its slots. */
#define OSI_QUEUE(name, n, size)                       \
    static uint8_t name##_items[(n) * (size)];         \
    static struct queue name = { .len = (n), .item_size = (size), .items = name##_items }

/* ETSTimer's layout: the driver keeps its armed timers listed through next. */
struct ets_timer {
    struct ets_timer *next;
    uint32_t expire;
    uint32_t period;
    void (*f)(void *);
    void *arg;
};

/* The adapter's start: its locks, its heap, and the watcher, interrupt and timer threads; 0 if one would not start. */
int osi_init(struct drv *d, struct self *s);
/* A thread more, which starts at f with arg on the stack of size bytes at stack, 16-aligned, kept for good. */
struct thread *osi_thread(void (*f)(void *), void *arg, const char *name, uint8_t *stack, uint32_t size);
struct thread *osi_adopt(const char *name, uintptr_t stack_lo, uintptr_t stack_hi);
uint32_t osi_stack_used(const struct thread *t);
struct thread *osi_self(void);
uint64_t osi_now_us(void);
/* The traced run's counters: the faults the watcher served, and the interrupts the adapter's thread ran. */
uint32_t osi_faults_served(void);
uint32_t osi_interrupts(void);
/* trace=1: the driver's steps and the adapter calls, in request order, and their dump; see osi.c. */
void osi_trace_init(uint32_t entries);
void osi_trace(const char *what, uint32_t a0, uint32_t a1);
void osi_trace_dump(void);
void osi_delay_ms(uint32_t ms);
void osi_timer_arm_us(struct ets_timer *t, uint32_t us);
void osi_timer_disarm(struct ets_timer *t);
int osi_queue_put(struct queue *q, const void *item, uint32_t ms);
int osi_queue_get(struct queue *q, void *item, uint32_t ms);
/* The interrupt's thread held off, as a critical section holds it; nested by the same thread. */
void osi_crit_enter(void);
void osi_crit_exit(void);
void osi_mutex_take(struct mutex *m);
void osi_mutex_give(struct mutex *m);
void drv_random(uint8_t *buf, size_t len);

/* The adapter's own steps of a start: the interrupt sources, the handler, its line, the Wi-Fi event,
 * the driver's event group, the chip's slow clock and coexistence PTI, and the coexistence turn-on. */
void osi_intr_set(int32_t cpu, uint32_t source, uint32_t n, int32_t prio);
void osi_isr_set(uint32_t n, void (*f)(void *), void *arg);
void osi_ints_on(uint32_t mask);
int32_t osi_event_post(const char *base, int32_t id, const void *data, size_t size);
uint32_t osi_event_set_bits(struct event_group *e, uint32_t bits);
uint32_t osi_event_wait_bits(struct event_group *e, uint32_t bits, int clear, int all, uint32_t ms);
uint32_t osi_slowclk_cal(void);
int osi_coex_enable(void);
int osi_coex_pti(uint32_t event, uint8_t *pti);

/*
 * heap.c: the heap, between the image's data and the ROM's at the top of the driver's block,
 * for the code that is not the driver's own and wants one: hostap's supplicant and Mbed TLS as a station joins.
 */
void osi_heap_init(const struct lock *l);
void *osi_malloc(size_t size);
void *osi_calloc(size_t n, size_t size);
void *osi_realloc(void *p, size_t size);
void osi_free(void *p);
uint32_t osi_heap_free(void);
uint32_t osi_heap_least(void); /* the fewest bytes free since the heap began */

/* phy.c: the modem's clocks, and the PHY Espressif's libphy.a brings up. */
void drv_phy_clock_enable(void);
void drv_phy_on(void); /* the PHY alone, as 802.15.4 wants it */
void drv_phy_off(void);
void drv_phy_enable(void); /* the PHY and Wi-Fi's part of it */
void drv_phy_disable(void);
void drv_phy_channel(uint32_t channel);
/* The PHY kept to the temperature, once a second while the station is joined; see phy.c. */
void drv_phy_track(void);
void drv_wifi_clock_enable(void);
void drv_wifi_reset_mac(void);

/* hostap.c: hostap's lines at its debug level too, from here on. */
void drv_wpa_debug(void);

/*
 * main.c: the driver's page, text to the kernel's log, its events,
 * and a step that failed, the first of which the page keeps for the root task.
 * A step that must succeed, the driver's own start's or mac.c's, stops the calling thread if it fails, CHILD_FAILED;
 * drv_stop stops any of the driver's threads.
 */
extern struct drv *drv_self;
void drv_say(const char *fmt, ...);
void drv_failed(const char *step, uint32_t detail);
void drv_vsay(const char *tag, const char *fmt, va_list args);
void drv_event(const char *base, int32_t id, const void *data, size_t size);
void drv_must(const char *step, esp_err_t e);
void drv_must_mac(const char *step, const char *failed);
__attribute__((noreturn)) void drv_stop(uint32_t state);
void drv_radio_off(void);
void drv_mac_told(const char *who);
void drv_trace(const char *way, const uint8_t *f, uint32_t len);

/* glue.c */
int vsnprintf(char *buf, size_t size, const char *fmt, va_list args);
int snprintf(char *buf, size_t size, const char *fmt, ...);

#endif
