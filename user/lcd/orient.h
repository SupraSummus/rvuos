#ifndef RVUOS_LCD_ORIENT_H
#define RVUOS_LCD_ORIENT_H

#include <stdint.h>

#include "st7789.h"

/*
 * A pattern that shows how MADCTL lays the picture on the glass, st7789_orient's bits.
 * On black, as the picture is laid out, from its own top left:
 * a white frame on its outermost pixels, whole only if the gap lies where st7789.c puts it,
 * a red square in its top left corner, a green one in its top right and a blue one in its bottom left,
 * and MADCTL's value in decimal beside the red, whose digits show a mirrored picture mirrored.
 * The squares lie ORIENT_INSET pixels in, clear of the glass's round corners.
 */
#define ORIENT_INSET  28u
#define ORIENT_SQUARE 24u
#define ORIENT_SCALE  3u

/* Every value orient_cycle shows: each of MY, MX and MV's eight, then BGR alone. */
#define ORIENT_SETTINGS 9u
extern const uint8_t orient_settings[ORIENT_SETTINGS];

/* A value of madctl that names no one setting but all of them, in turn. */
#define ORIENT_ALL 256u

/* How long orient_cycle holds each setting, in microseconds, for a person to hold the glass beside the simulator's. */
#define ORIENT_HOLD_US 5000000u

void orient_draw(struct st7789 *lcd, uint8_t madctl);

/* Each of orient_settings in turn, held ORIENT_HOLD_US, the picture laid out as MADCTL 0 after. */
void orient_cycle(struct st7789 *lcd);

#endif
