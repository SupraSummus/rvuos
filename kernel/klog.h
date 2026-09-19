#ifndef RVUOS_KLOG_H
#define RVUOS_KLOG_H

#include <stdbool.h>
#include <stdint.h>

#include "kernel.h"

/*
 * The kernel's log: a ring of bytes in the kernel's memory,
 * which the root task reads through BOOT_CAP_LOG
 * and which raises LOG_IRQ_LINE while it holds bytes the reader has not taken.
 * The kernel has no console; this is where every byte it prints goes.
 * See DESIGN.md, "The kernel log", and struct rvuos_log in rvuos/abi.h.
 */

/* The region BOOT_CAP_LOG names: the header and the ring, at KLOG_BASE. */
#define KLOG_REGION_SIZE (RVUOS_LOG_HEADER + KLOG_SIZE)

struct irq;

/*
 * Place the log at KLOG_BASE.
 * The header is reset; the ring's contents are left as they are,
 * so a boot that does not clear RAM finds the previous run's last words there.
 */
void klog_init(void);

/* Append one byte, and raise the line if the reader had taken everything so far. */
void klog_append(char c);

/* OP_IRQ_SET on the log's line, the bits already set: signals at once if the line is high. */
void klog_set(struct irq *irq);

/* True while the head lies past what the reader has taken; for the self-check. */
bool klog_pending(void);

/* Hand the bytes the reader has not taken, oldest first, to `put`; for a board's halt. */
void klog_dump(void (*put)(char));

#endif
