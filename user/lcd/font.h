#ifndef RVUOS_LCD_FONT_H
#define RVUOS_LCD_FONT_H

#include <stdint.h>

#include "st7789.h"

/*
 * Digits of 3 by 5 dots, each dot scale pixels square, with a dot's gap between digits,
 * which a scene writes numbers with.
 */
#define FONT_W 3u
#define FONT_H 5u

/* Row row of digit's dots, the left one in bit FONT_W - 1, for a scene that streams its rows. */
uint32_t font_row(uint32_t digit, uint32_t row);

/* n in decimal, its top left at x, y of the panel, in colour. */
void font_number(const struct st7789 *lcd, uint32_t x, uint32_t y, uint32_t n, uint32_t scale, uint16_t colour);

#endif
