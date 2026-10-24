#ifndef RVUOS_USER_CONSOLE_H
#define RVUOS_USER_CONSOLE_H

/*
 * The console of the ESP32-C6: the USB Serial/JTAG controller, on line 49,
 * and a line nothing drives, FROM_CPU_INTR3's, 26, which only software raises.
 * A line is an interrupt matrix source plus one; see kernel/board/esp32c6/irq.c.
 */
#define CONSOLE_IRQ 49
#define SPARE_IRQ   26

#include "usj.h"

#endif
