#ifndef RVUOS_USER_CONSOLE_H
#define RVUOS_USER_CONSOLE_H

/*
 * The console of mps2-an521: UART0, its transmitter on its combined line, 42, a line nothing drives, 31,
 * and MHU0's line, 6, which the kernel keeps once it runs on both cores.
 */
#define CONSOLE_IRQ 42
#define SPARE_IRQ   31
#define CORES_IRQ   6

#include "cmsdk-uart.h"

#endif
