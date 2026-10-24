#ifndef RVUOS_LCD_ST7789_H
#define RVUOS_LCD_ST7789_H

/*
 * The LCD of Waveshare's ESP32-C5-LCD-1.47: an ST7789 driving a panel of 172 by 320 pixels,
 * through any bus.h, so that the same code runs on the board and against the host's model.
 *
 * The controller holds 240 by 320 pixels, of which the panel shows columns 34 to 205,
 * as Waveshare's own support package has it, and the board shows it;
 * a program names the panel's pixels, from 0 to LCD_W - 1 and to LCD_H - 1, and this adds the gap.
 * Colours are RGB565, which goes out high byte first.
 */

#include <stdint.h>

#define LCD_W     172u
#define LCD_H     320u
#define LCD_X_GAP 34u

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xf8u) << 8) | (((g) & 0xfcu) << 3) | ((b) >> 3)))

/*
 * Reset the controller and wake it, in RGB565, rows top to bottom and columns left to right,
 * with its colours inverted, which this panel shows as written, and the display on.
 * What the controller holds after it is whatever its memory held, so a program fills the screen first.
 */
void st7789_init(void);

/* Fill the rectangle [x0, x1] by [y0, y1] of the panel with one colour. */
void st7789_fill(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint16_t colour);

#endif
