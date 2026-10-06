#ifndef RVUOS_USER_CONSOLE_H
#define RVUOS_USER_CONSOLE_H

/*
 * The console of the ESP32-C6: the USB Serial/JTAG controller behind BOOT_CAP_UART,
 * a CDC-ACM serial port on the chip's own USB, on line 49,
 * and a line nothing drives, FROM_CPU_INTR3's, which only software raises.
 * A line is an interrupt matrix source plus one; see kernel/board/esp32c6/irq.c.
 * Each board's console.h offers the same functions, for user/init/ and user/fuzzdrv.c.
 *
 * Bytes go into a 64-byte FIFO, the IN endpoint,
 * and go out as one USB packet when WR_DONE is written.
 * The host takes a packet only while it has the port open;
 * until then the FIFO stays full and the console waits.
 * What the host sends comes into the OUT endpoint, which console_get reads, the ESP32-C6's console alone.
 */

#include <stdbool.h>
#include <stdint.h>

#define CONSOLE_IRQ 49
#define SPARE_IRQ   26

#define USJ_EP1      0 /* the IN FIFO, one byte per write */
#define USJ_EP1_CONF 1
#define USJ_INT_ST   3
#define USJ_INT_ENA  4
#define USJ_INT_CLR  5
#define USJ_WR_DONE  0x1u /* in EP1_CONF: send what the FIFO holds */
#define USJ_FREE     0x2u /* in EP1_CONF: the FIFO takes another byte */
#define USJ_OUT_DATA 0x4u /* in EP1_CONF: the OUT FIFO holds a byte the host sent */
#define USJ_OUT_RECV 0x4u /* interrupt: a packet came from the host */
#define USJ_IN_EMPTY 0x8u /* interrupt: the host has taken everything */

/* Polls before the polled writer gives up on a host that takes nothing. */
#define USJ_POLLS 1000000u

static volatile uint32_t *console_regs;
static bool console_dead;

static inline void console_init(uint32_t base)
{
    console_regs = (volatile uint32_t *)base;
}

static inline void console_flush(void)
{
    console_regs[USJ_EP1_CONF] = USJ_WR_DONE;
}

/*
 * One byte out by polling, sending the FIFO when it is full.
 * With no host the FIFO never empties, so after one long wait
 * the writer drops this byte and every later one rather than hang.
 */
static inline void console_put_polled(char c)
{
    for (uint32_t n = 0; !console_dead && (console_regs[USJ_EP1_CONF] & USJ_FREE) == 0; n++) {
        if (n == 0) {
            console_flush();
        }
        if (n == USJ_POLLS) {
            console_dead = true;
        }
    }
    if (!console_dead) {
        console_regs[USJ_EP1] = (uint8_t)c;
    }
}

/* From here the controller raises the line once the host has taken what was sent. */
static inline void console_start(void)
{
    console_regs[USJ_INT_CLR] = USJ_IN_EMPTY;
    console_regs[USJ_INT_ENA] = USJ_IN_EMPTY;
}

/*
 * One byte out, waiting on the interrupt only while the FIFO is full.
 * `wait` arms the console's Irq and returns when it has signalled.
 * The empty bit is an event the controller latches,
 * so it is cleared before the FIFO is sent and not after,
 * or a host quicker than the clear would leave nothing to wake on.
 * False if the interrupt was for something else.
 */
static inline bool console_put(char c, void (*wait)(void))
{
    while ((console_regs[USJ_EP1_CONF] & USJ_FREE) == 0) {
        console_regs[USJ_INT_CLR] = USJ_IN_EMPTY;
        console_flush();
        wait();
        if ((console_regs[USJ_INT_ST] & USJ_IN_EMPTY) == 0) {
            return false;
        }
    }
    console_regs[USJ_EP1] = (uint8_t)c;
    return true;
}

/*
 * From here the controller raises the line when the host sends a packet, besides what console_start asked.
 * The packet's bit is an event the controller latches, so it is cleared before the FIFO is read, not after.
 */
static inline void console_listen(void)
{
    console_regs[USJ_INT_CLR] = USJ_OUT_RECV;
    console_regs[USJ_INT_ENA] |= USJ_OUT_RECV;
}

/* The next byte the host sent, into c: whether one was there. */
static inline bool console_get(char *c)
{
    if ((console_regs[USJ_EP1_CONF] & USJ_OUT_DATA) == 0) {
        return false;
    }
    *c = (char)console_regs[USJ_EP1];
    return true;
}

#endif
