#ifndef RVUOS_WIFI_OSI_H
#define RVUOS_WIFI_OSI_H

/* The driver's pieces that Espressif's libraries reach through the adapter, and that reach each other. */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "drv.h"
#include "esp.h"
#include "lib/lock.h"
#include "lib/self.h"

struct thread;
struct ets_timer;
struct queue;
struct mutex;

struct osi_isr {
    void (*f)(void *);
    void *arg;
};

/*
 * osi.c: the adapter, its table for esp_wifi_init_internal, the threads it runs everything on,
 * built from the driver's own account s, its timers, which its timer thread runs,
 * and the chip's random number generator.
 * A thread that did not start as one of the adapter's, the driver's first, joins them with osi_adopt,
 * its stack between stack_lo and stack_hi, before it calls the libraries.
 * osi_stack_used says the deepest a thread osi_thread made has gone into its stack.
 * osi_isr_swap exchanges the handler of an interrupt the libraries handle with one of the driver's own, mac.c's.
 * The driver's own code waits on the adapter's queues and mutexes too, as the libraries do, by these:
 * a wait of OSI_FOREVER milliseconds ends only when it is met, and one of 0 does not wait;
 * a put or a get returns 1 if it was done.
 */
#define OSI_FOREVER 0xffffffffu

struct osi_funcs *osi_init(struct drv *d, struct self *s);
void osi_log_level(unsigned int level);
struct thread *osi_thread(void (*f)(void *), void *arg, const char *name, uint32_t stack);
struct thread *osi_adopt(const char *name, uintptr_t stack_lo, uintptr_t stack_hi);
uint32_t osi_stack_used(const struct thread *t);
struct thread *osi_self(void);
uint64_t osi_now_us(void);
void osi_delay_ms(uint32_t ms);
int osi_isr_swap(uint32_t source, struct osi_isr *isr);
struct ets_timer *osi_timer_new(void (*f)(void *), void *arg);
void osi_timer_arm_us(struct ets_timer *t, uint32_t us);
void osi_timer_disarm(struct ets_timer *t);
struct queue *osi_queue_new(uint32_t len, uint32_t item_size);
int osi_queue_put(struct queue *q, const void *item, uint32_t ms);
int osi_queue_get(struct queue *q, void *item, uint32_t ms);
struct mutex *osi_mutex_new(void);
void osi_mutex_take(struct mutex *m);
void osi_mutex_give(struct mutex *m);
void drv_random(uint8_t *buf, size_t len);

/* heap.c: the heap, between the image's data and the ROM's at the top of the driver's block. */
void osi_heap_init(const struct lock *l);
void *osi_malloc(size_t size);
void *osi_calloc(size_t n, size_t size);
void *osi_realloc(void *p, size_t size);
void osi_free(void *p);
uint32_t osi_heap_free(void);
uint32_t osi_heap_least(void); /* the fewest bytes free since the heap began */

/* phy.c: the modem's clocks, and the PHY Espressif's libphy.a brings up. */
void drv_phy_clock_enable(void);
void drv_phy_enable(void);
void drv_phy_disable(void);
void drv_phy_channel(uint32_t channel);
void drv_wifi_clock_enable(void);
void drv_wifi_reset_mac(void);

/* wpa.c: the supplicant, supp.h, behind the table the libraries call a supplicant through. */
esp_err_t drv_wpa_register(void);

/* hostap.c: hostap's lines at its debug level too, from here on. */
void drv_wpa_debug(void);

/* crypto.c: hostap's crypto, in the table the libraries call crypto through. */
void drv_crypto_funcs(struct crypto_funcs *c);

/*
 * main.c: the driver's page, text to the kernel's log, the libraries' events,
 * and a step that failed, the first of which the page keeps for the root task.
 * A step that must succeed, the libraries' or mac.c's, stops the calling thread if it fails, CHILD_FAILED;
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
