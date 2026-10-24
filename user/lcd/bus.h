#ifndef RVUOS_LCD_BUS_H
#define RVUOS_LCD_BUS_H

/*
 * The bus an ST7789 hangs on, as the panel's code sees it:
 * bytes out, a pin that says whether a byte is a command or data, a select that frames them, and two plain pins.
 * The board's bus is spi2.c, SPI2 and the ESP32-C5's pins;
 * the host's is the model of the controller in sim/panel.c, which runs the same code above this line on a host.
 *
 * A command byte goes out with the select low and DC low, and leaves the select low;
 * the bytes after it go out with DC high, its parameters or its pixels, as many as there are;
 * lcd_bus_end raises the select, which ends the command.
 */

#include <stdint.h>

/* Set the bus up: the pins idle, the controller deselected and out of reset, the backlight dark. */
void lcd_bus_init(void);

/* Start a command: the select low, and the byte out with DC low. */
void lcd_bus_command(uint8_t command);

/*
 * The command's data, n bytes with DC high, in order.
 * A buffer aligned to four bytes goes out a word at a time, which spi2.c packs fastest.
 */
void lcd_bus_data(const uint8_t *bytes, uint32_t n);

/* End the command: the select high. */
void lcd_bus_end(void);

/*
 * The next 32 bits the controller sends, after a command and before lcd_bus_end, the first in the top bit.
 * Whether a command's answer starts with a dummy clock is the command's, so the caller shifts what it takes.
 * The bus clocks a read no faster than the controller can answer, 6.6 MHz.
 */
uint32_t lcd_bus_read(void);

/* The controller's reset pin: held in reset while low is nonzero. */
void lcd_bus_reset(int low);

/* The backlight: dark at 0, lit whole at LCD_BACKLIGHT_FULL, and between, lit for level of every 256 parts of the time. */
#define LCD_BACKLIGHT_FULL 255u
void lcd_bus_backlight(uint32_t level);

/* Wait at least us microseconds. */
void lcd_bus_sleep(uint32_t us);

#endif
