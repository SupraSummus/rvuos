/*
 * The ROM's delay, ets_delay_us, which counts in mcycle, a CSR user mode cannot read on the ESP32-C5:
 * this one waits on the kernel's clock instead, and the ROM's linker script only provides the name,
 * so every caller the link resolves takes this one; the ROM's own functions still call the ROM's.
 * A wait is at least as long as asked, and longer by up to a clock read, a system call.
 */

#include <stdint.h>

#include "drv.h"
#include "osi.h"
#include "rvuos.h"

void ets_delay_us(uint32_t us);

void ets_delay_us(uint32_t us)
{
    uint64_t start, now;
    uint32_t hz;
    rv_clock_read(drv_self->clock, &start, &hz);
    uint64_t ticks = (uint64_t)us * hz / 1000000u;
    do {
        rv_clock_read(drv_self->clock, &now, &hz);
    } while (now - start < ticks);
}
