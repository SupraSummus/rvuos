#ifndef RVUOS_WIFI_GSPI_H
#define RVUOS_WIFI_GSPI_H

/*
 * The CYW43439's gSPI bus as the Pico 2 W wires it: one data line both ways on GPIO24,
 * its clock on GPIO29, chip select on GPIO25 and the chip's power, WL_ON, on GPIO23.
 * PIO0's first state machine clocks the bits; the CPU feeds and drains its FIFOs,
 * which stall the clock when they run empty or full, so no DMA is needed and no bit is lost.
 * Chip select and power are plain outputs, driven through the pins' output overrides in IO_BANK0,
 * since a process reaches no SIO.
 */

#include <stdint.h>

#define GSPI_PIN_ON   23u
#define GSPI_PIN_DATA 24u
#define GSPI_PIN_CS   25u
#define GSPI_PIN_CLK  29u

struct gspi {
    uintptr_t pio;  /* PIO0's registers */
    uintptr_t pins; /* IO_BANK0's control registers of GPIO16 to GPIO31 */
};

/* Loads the program and sets the state machine up; the pins' functions and pads are the caller's. */
void gspi_init(struct gspi *bus, uintptr_t pio, uintptr_t pins);
/* The chip's power, and the data line held low, as it must be while the power rises. */
void gspi_power(struct gspi *bus, int on);
void gspi_hold_data_low(struct gspi *bus);
/* Sends words, the command first, and returns the status word the chip answers with. */
uint32_t gspi_write(struct gspi *bus, const uint32_t *words, uint32_t count);
/* Sends a command and reads count words of answer, returning the status word. */
uint32_t gspi_read(struct gspi *bus, uint32_t cmd, uint32_t *words, uint32_t count);

#endif
