#ifndef RVUOS_USER_PINS_H
#define RVUOS_USER_PINS_H

/*
 * The ESP32-C5's pins, through the IO MUX and the GPIO matrix of devices.h's BOOT_CAP_IO_MUX and BOOT_CAP_GPIO,
 * as ESP-IDF's soc/esp32c5 lays them out; the ESP32-C6's pins.h names the same, laid out as its chip has them.
 */

#include "devices.h"

/* The GPIO matrix: levels, output enables, each pin's output signal, and each input signal's pin. */
#define GPIO_OUT_W1TS       (GPIO_BASE + 0x08u)
#define GPIO_OUT_W1TC       (GPIO_BASE + 0x0cu)
#define GPIO_ENABLE_W1TS    (GPIO_BASE + 0x38u)
#define GPIO_ENABLE_W1TC    (GPIO_BASE + 0x3cu)
#define GPIO_IN_SEL(sig)    (GPIO_BASE + 0x2d4u + 4u * (sig))
#define GPIO_IN_MATRIX      (1u << 8)  /* the signal comes from the pin named, through the matrix */
#define GPIO_OUT_SEL(n)     (GPIO_BASE + 0xad4u + 4u * (n))
#define GPIO_OE_FROM_ENABLE (1u << 10) /* the pin is driven while its GPIO_ENABLE bit is set */
#define SIG_GPIO_OUT        256u       /* the pin's level is its GPIO_OUT bit */
#define SIG_FSPICLK         56u
#define SIG_FSPIQ           57u        /* SPI2's MISO, an input */
#define SIG_FSPID           58u

/* The IO MUX: each pin's function, 1 the GPIO matrix's, its drive strength, of 0 to 3, and its input. */
#define IO_MUX_PIN(n)        (IO_MUX_BASE + 4u * (n))
#define IO_MUX_MATRIX(drive) ((1u << 12) | ((drive) << 10))
#define IO_MUX_INPUT         (1u << 9)

#endif
