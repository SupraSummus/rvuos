#ifndef RVUOS_USER_CONSOLE_H
#define RVUOS_USER_CONSOLE_H

/*
 * The console of nRF52840: a block of RAM behind BOOT_CAP_UART, which tools/nrf52840-run.py reads over SWD
 * as the core runs; see kernel/board/nrf52840/board.h.
 * Its first word counts every byte written, and byte n lies at offset 16 plus n modulo the room,
 * so the block is a ring the runner follows, and tells how many it lost when it fell a whole ring behind.
 * Nothing raises a line for it, so its line and the spare one are two IRQs no device drives unless told to,
 * SWI0_EGU0 and SWI1_EGU1, whose event generators nothing starts.
 * Each board's console.h offers the same functions, for user/init/ and user/fuzzdrv.c.
 */

#include <stdbool.h>
#include <stdint.h>

#define CONSOLE_IRQ 20
#define SPARE_IRQ   21

/* The block's size, UART_SIZE in the kernel's board.h, less the header. */
#define CONSOLE_ROOM (0x2000u - 16u)

static volatile uint32_t *console_head;
static volatile uint8_t *console_bytes;

static inline void console_init(uint32_t base)
{
    console_head = (volatile uint32_t *)base;
    console_bytes = (volatile uint8_t *)(base + 16u);
}

/* The ring always takes a byte, so the polled writer and the other are one. */
static inline void console_put_polled(char c)
{
    uint32_t head = *console_head;
    console_bytes[head % CONSOLE_ROOM] = (uint8_t)c;
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
