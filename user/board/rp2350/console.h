#ifndef RVUOS_USER_CONSOLE_H
#define RVUOS_USER_CONSOLE_H

/*
 * The console of RP2350: a block of RAM behind BOOT_CAP_UART, which the kernel sends to the host when it halts;
 * see kernel/board/rp2350/halt.c.
 * Its first word counts every byte written, and the bytes follow from offset 16, as many as fit;
 * the ones past the end are counted and dropped.
 * Nothing raises a line for it, so its line and the spare one are two IRQs no device drives,
 * SPAREIRQ_IRQ_0 and SPAREIRQ_IRQ_1, which only software can force.
 * On the Cortex-M33 SIO's doorbell line, 26, is the kernel's once it runs on both cores;
 * Hazard3's cores interrupt each other on no line.
 * Each board's console.h offers the same functions, for user/init.c and user/fuzzdrv.c.
 */

#include <stdbool.h>
#include <stdint.h>

#define CONSOLE_IRQ 46
#define SPARE_IRQ   47
#ifndef __riscv
#define CORES_IRQ   26
#endif

/* The block's size, UART_SIZE in the kernel's board.h, less the header. */
#define CONSOLE_ROOM (0x2000u - 16u)

static volatile uint32_t *console_head;
static volatile uint8_t *console_bytes;

static inline void console_init(uint32_t base)
{
    console_head = (volatile uint32_t *)base;
    console_bytes = (volatile uint8_t *)(base + 16u);
}

/* The block always takes a byte, so the polled writer and the other are one. */
static inline void console_put_polled(char c)
{
    uint32_t head = *console_head;
    if (head < CONSOLE_ROOM) {
        console_bytes[head] = (uint8_t)c;
    }
    *console_head = head + 1;
}

static inline void console_flush(void)
{
}

static inline void console_start(void)
{
}

/* One byte out; it never waits, so `wait` is never called. */
static inline bool console_put(char c, void (*wait)(void))
{
    (void)wait;
    console_put_polled(c);
    return true;
}

#endif
