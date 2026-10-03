#ifndef RVUOS_LIB_LOG_H
#define RVUOS_LIB_LOG_H

/*
 * The kernel's log, from both sides: text a process writes into it through a Debug capability,
 * and the reader's view, which the boot grants the root task.
 */

#include <stdint.h>

#include "lib/say.h"
#include "rvuos.h"

struct self;

/*
 * Text into the kernel's log through the Debug capability in slot debug, which needs RIGHT_W alone,
 * in order with the kernel's own lines and every other writer's, DEBUG_WRITE_BYTES a call.
 */
void log_write(void *debug, const char *s, uint32_t n);
static inline struct out log_out(uint32_t debug)
{
    return (struct out){ log_write, (void *)(uintptr_t)debug };
}

/*
 * The kernel's log, see struct rvuos_log, as its reader sees it:
 * installed in a region of the reader's, with an Irq on LOG_IRQ_LINE that signals the reader's inbox
 * while the log holds bytes it has not taken.
 */
struct kernel_log {
    volatile struct rvuos_log *header;
    const volatile uint8_t *ring;
    uint32_t taken;
    uint32_t irq;
};

/* Installs BOOT_CAP_LOG and binds its line; root task only. */
uint32_t kernel_log_open(struct self *s, struct kernel_log *k);
/* Has the line signal bit once the log holds bytes not taken, at once if it does already. */
uint32_t kernel_log_arm(const struct kernel_log *k, uint32_t bit);
/* Hands put every byte the kernel wrote since the last call, and tells the kernel how far it was read. */
void kernel_log_take(struct kernel_log *k, void (*put)(char));

#endif
