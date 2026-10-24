#ifndef RVUOS_LCD_STRESS_H
#define RVUOS_LCD_STRESS_H

/*
 * A pattern that shows what goes wrong on a bus driven near its limit, four bands from the top,
 * each for a failure a person sees on the glass, or a camera does, without telling one pixel from the next:
 *   - a gradient, red across and green down, every pixel its own value, in one stream:
 *     a bit stuck or lost shows as a step in what should be smooth;
 *   - a grid of squares of 8 by 8 on black, each its own window and command:
 *     a command misread, or a byte taken for a command, shows as a square missing or out of line;
 *   - a checkerboard of single pixels, 0xAAAA and 0x5555 on the left, whose bits change on every clock,
 *     the hardest the data line is driven, and white and black on the right: an error shows as a speck of colour;
 *   - stripes 4 pixels wide, red, green, blue and white, in one stream:
 *     a clock lost or gained shifts every bit after it, so from there on the stripes turn to other colours.
 */
void stress_draw(void);

#endif
