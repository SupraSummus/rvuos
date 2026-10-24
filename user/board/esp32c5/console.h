#ifndef RVUOS_USER_CONSOLE_H
#define RVUOS_USER_CONSOLE_H

/*
 * The console of the ESP32-C5: the USB Serial/JTAG controller, on line 55,
 * and a line nothing drives, FROM_CPU_INTR3's, 27, which only software raises.
 * A line is an interrupt matrix source plus one; see kernel/board/esp32c5/irq.c.
 */
#define CONSOLE_IRQ 55
#define SPARE_IRQ   27

#include "usj.h"

#endif
