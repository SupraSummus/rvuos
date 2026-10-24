#ifndef RVUOS_LCD_PATTERN_H
#define RVUOS_LCD_PATTERN_H

/*
 * The test pattern, which shows at a glance whether the panel is set up right:
 * three bands, red, green and blue, from the top, which show the order of the colours and whether they are inverted,
 * eight bands of grey from black to white, which show the shades,
 * and a white frame one pixel inside the edge, which shows whether the window lies where the panel is.
 */
void pattern_draw(void);

#endif
