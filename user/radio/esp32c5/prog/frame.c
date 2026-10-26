/*
 * The modem's frame as a program finds it: every word of 0x600A0000 to 0x600B0000 read from user mode,
 * through the frame the kernel lists and the root task maps, and told a 1 KiB page a line:
 * how many of its words read other than zero, and the first of them, with the microseconds the page's reads took,
 * since a block whose clock is off answers only after the bus has waited.
 * It only reads, and sends nothing; its run ends when the root task asks it to leave.
 *
 * Run it with make BOARD=esp32c5 radio PROG=frame RADIO_CONFIG=file, see build.mk.
 */

#include <stdint.h>

#include "devices.h"
#include "drv.h"
#include "osi.h"
#include "rvuos.h"

DRV_PROGRAM("the modem's frame", 0);

#define FRAME_SIZE 0x10000u
#define PAGE_SIZE  0x400u
#define BURST      16u /* lines a burst, and a pause after each, for the root task to carry them out */
#define PAUSE_MS   10u

void drv_radio_off(void)
{
}

/* The kernel's clock in microseconds, since user mode reads no counter of the core's on this chip. */
static uint32_t now_us(uint32_t clock)
{
    uint64_t now;
    uint32_t hz;
    rv_clock_read(clock, &now, &hz);
    return (uint32_t)(now * 1000000u / hz);
}

void drv_run(struct drv *d, struct self *own)
{
    (void)own;
    child_report(&d->c, DRV_LISTENING);
    uint32_t told = 0;
    for (uint32_t page = MODEM_BASE; page < MODEM_BASE + FRAME_SIZE; page += PAGE_SIZE) {
        uint32_t nonzero = 0, first_at = 0, first = 0;
        uint32_t t0 = now_us(d->clock);
        for (uint32_t at = page; at < page + PAGE_SIZE; at += 4) {
            uint32_t w = *(volatile uint32_t *)at;
            if (w != 0 && nonzero++ == 0) {
                first_at = at;
                first = w;
            }
        }
        uint32_t t = now_us(d->clock) - t0;
        drv_say("frame: %x %u words, first %x = %x, %u us\n", page, nonzero, first_at, first, t);
        if (++told == BURST) {
            told = 0;
            osi_delay_ms(PAUSE_MS);
        }
    }
    drv_say("frame: the frame is told\n");
    for (;;) {
        uint32_t bits;
        child_answer(&d->c);
        if (d->leave) {
            break;
        }
        rv_wait(CHILD_INBOX, &bits);
    }
    child_stop(&d->c, DRV_LEFT);
}
