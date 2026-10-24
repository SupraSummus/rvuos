#ifndef RVUOS_LCD_SIM_PANEL_H
#define RVUOS_LCD_SIM_PANEL_H

/*
 * A model of the LCD of Waveshare's ESP32-C5-LCD-1.47, the host's bus.h:
 * an ST7789 as its datasheet has it, and the panel behind it as the board shows it.
 * It takes the bytes the program's code sends, as the controller would on its pins,
 * keeps the controller's memory and state, and says what the glass shows.
 * What it does not model it refuses, as an error, rather than guess,
 * and it checks the datasheet's rules of order and timing, each broken one an error;
 * see panel.c for what is modelled, and which of it the board has confirmed.
 *
 * Time is simulated: it passes with each byte on the bus, at the board's clock, and with each sleep.
 */

#include <stdint.h>

/* The glass: what the panel shows of the controller's memory. */
#define GLASS_W 172u
#define GLASS_H 320u

/*
 * What the glass shows at x, y, as 0xRRGGBB, dimmed or dark where its round corners hide the pixel:
 * what the memory held when the refresh last passed the row, at most a frame ago.
 */
uint32_t panel_shown(uint32_t x, uint32_t y);

/* What the controller's memory holds at x, y of its 240 by 320, as 0xRRGGBB, and whether the glass shows it. */
uint32_t panel_memory(uint32_t x, uint32_t y, int *on_glass);
#define MEMORY_W 240u
#define MEMORY_H 320u

/* How many times the refresh has passed the glass's last row since the bus was set up. */
uint64_t panel_refreshes(void);

/* The errors the model met so far; each was written to stderr as it was met. */
uint32_t panel_errors(void);

/* The simulated time since lcd_bus_init, in nanoseconds, and the bytes sent on the bus. */
uint64_t panel_now_ns(void);
uint64_t panel_bytes(void);

/* The bus's clock, 40 MHz unless set, which lcd_bus_init keeps. */
void panel_bus_mhz(uint32_t mhz);

/* Called whenever simulated time passes, with the time it has reached, for a recorder of frames; 0 for none. */
void panel_on_time(void (*hook)(uint64_t now_ns));

#endif
