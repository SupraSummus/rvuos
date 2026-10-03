#ifndef RVUOS_LIB_LOG_H
#define RVUOS_LIB_LOG_H

/*
 * Logs: a ring of text a process writes for another to copy out, and the kernel's, which the boot grants the root task.
 */

#include <stdint.h>

#include "rvuos.h"

struct self;

/*
 * A ring of text in memory a writer shares with a reader, as a child's page holds its log, see child.h:
 * head counts every byte the writer put, and byte n lies at bytes[n % LOGRING_SIZE].
 * The reader keeps its own count, and finds itself overtaken when head runs more than LOGRING_SIZE ahead.
 */
#define LOGRING_SIZE 1536u
struct logring {
    volatile uint32_t head;
    char bytes[LOGRING_SIZE];
};

/* Puts a byte; 1 at the end of a line, when the reader is to be told. */
int logring_put(struct logring *l, char c);
/* Hands the reader every byte past taken, and returns the new count. */
uint32_t logring_take(const struct logring *l, uint32_t taken, void (*put)(char));

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
