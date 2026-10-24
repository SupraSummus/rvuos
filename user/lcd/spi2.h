#ifndef RVUOS_LCD_SPI2_H
#define RVUOS_LCD_SPI2_H

#include <stdint.h>

/*
 * The board's bus beyond bus.h: its clock, 40 MHz, within the ST7789's 62, or 80, the kernel's clock undivided and past it,
 * and the drive of its pins, 0 the weakest to 3, whose slower edges leave the bus less margin.
 * lcd_bus_init sets 40 MHz and drive 2.
 * The board took 80 MHz with no error the maintainer could see, on 2026-10-24, with stress.h's pattern,
 * at drive 2 and at drive 0, so this one board has a margin at 80;
 * a board is not the datasheet, so 40 stays the default.
 */
void spi2_configure(uint32_t mhz, uint32_t drive);

/*
 * Send data from the size bytes at base by DMA from now on, through the Dma in slot,
 * its source a frame over those bytes and its sink SPI2, or by the CPU again for slot 0.
 * Data that lies there, whole words of it, goes out with no more of the CPU than a start;
 * the rest goes through SPI2's buffer as before.
 */
void spi2_dma(uint32_t slot, const void *base, uint32_t size);

/* What spi2_dma set, which the root task keeps, as only it keeps globals; see user/build.mk. */
struct spi2_dma {
    uint32_t slot;
    uintptr_t base;
    uint32_t size;
};
extern struct spi2_dma spi2_dma_state;

#endif
