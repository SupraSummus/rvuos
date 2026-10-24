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

#endif
