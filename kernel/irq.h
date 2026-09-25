#ifndef RVUOS_IRQ_H
#define RVUOS_IRQ_H

#include <stdbool.h>
#include <stdint.h>

#include "layout.h"

/*
 * The interrupt controller, which delivers device interrupts as lines.
 * The kernel owns it and userspace sees lines only through Irq objects;
 * see DESIGN.md, "Interrupts".
 * Where the controller lives and how many lines it has is the board's business,
 * so irq.c is per board, in kernel/board/<board>/, as timer.c and halt.c are.
 */

/*
 * The controller's line identifiers lie below IRQ_LINES, which the board's board.h sets.
 * Line 0 is the kernel's log, see klog.h, whatever the controller has there.
 * The TIMER_LINES timer lines follow from IRQ_LINES, and the tick raises them;
 * see DESIGN.md, "Time".
 * The functions below are called only with a line on the controller.
 */
#define LINES (IRQ_LINES + TIMER_LINES)

static inline bool line_is_timer(uint32_t line) { return line >= IRQ_LINES; }
static inline bool line_on_controller(uint32_t line) { return line != LOG_IRQ_LINE && line < IRQ_LINES; }

/* Mask every line and enable the external interrupt at the core. */
void irq_init(void);

/* Have the controller forward a line to the core, or stop it. */
void irq_enable(uint32_t line, bool on);

/* Read back whether the controller forwards a line; for the self-check. */
bool irq_enabled(uint32_t line);

/*
 * Take the line of a pending, unmasked interrupt from the controller, 0 for none.
 * The controller holds the line as claimed until irq_complete,
 * and the kernel masks it in between,
 * so a level that stays high does not come back before the driver asks for it.
 */
uint32_t irq_claim(void);
void irq_complete(uint32_t line);

#endif
