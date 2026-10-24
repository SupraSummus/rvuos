#ifndef RVUOS_LCD_ST7789_H
#define RVUOS_LCD_ST7789_H

/*
 * The LCD of Waveshare's ESP32-C5-LCD-1.47: an ST7789 driving a panel of 172 by 320 pixels,
 * through any bus.h, so that the same code runs on the board and against the host's model.
 *
 * The controller holds 240 by 320 pixels, of which the panel shows columns 34 to 205,
 * as Waveshare's own support package has it, and the board shows it;
 * a program names the panel's pixels, from 0 to LCD_W - 1 and to LCD_H - 1, and this adds the gap,
 * or, once st7789_orient has turned the picture, from 0 to st7789_width() - 1 and to st7789_height() - 1.
 * How the picture lies is the program's, in a struct st7789 it keeps and passes, as a process holds no globals.
 * Colours are RGB565, which goes out high byte first.
 */

#include <stdint.h>

#define LCD_W     172u
#define LCD_H     320u
#define LCD_X_GAP 34u

/* MADCTL's bits: the rows' order, the columns', rows and columns exchanged, and blue and red exchanged. */
#define ST7789_MY  0x80u
#define ST7789_MX  0x40u
#define ST7789_MV  0x20u
#define ST7789_BGR 0x08u

/* How the program has the controller lay the picture out: MADCTL as st7789_orient last set it. */
struct st7789 {
    uint8_t madctl;
};

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xf8u) << 8) | (((g) & 0xfcu) << 3) | ((b) >> 3)))

/*
 * Reset the controller and wake it, in RGB565, rows top to bottom and columns left to right,
 * with its colours inverted, which this panel shows as written, and the display on.
 * What the controller holds after it is whatever its memory held, so a program fills the screen first.
 */
void st7789_init(struct st7789 *lcd);

/*
 * The controller's refresh, as the board showed it on 2026-10-24, through GSCAN, with FRCTRL2 as it starts:
 * a frame of ST7789_FRAME_US, 60.08 Hz, of ST7789_LINES lines, the panel's 320 rows and 14 of porch,
 * each 49.8 microseconds; which of the lines are the porch the board did not show.
 */
#define ST7789_LINES    334u
#define ST7789_FRAME_US 16645u

/* The line the controller is refreshing, 0 to ST7789_LINES - 1, which reads of GSCAN tell. */
uint32_t st7789_scanline(void);

/*
 * Wait until the refresh is at line ST7789_SYNC_FIRST to ST7789_SYNC_LAST, where a picture written whole, top row first,
 * in 9.2 to 24.6 ms starts without tearing wherever the porch lies: the refresh is then s rows in, 136 to 160,
 * and a picture of w a row, against the refresh's l of 49.8 us, stays clear while s >= 320 (1 - w / l), as at 80 MHz,
 * or, written slower, as at 40 MHz, shows whole the next time round while 319 (w - l) <= (334 - s) l.
 * It gives up after two frames' worth of reads.
 */
#define ST7789_SYNC_FIRST 150u
#define ST7789_SYNC_LAST  160u
void st7789_sync(void);

/*
 * Lay the picture on the panel as MADCTL's bits say, 0 as st7789_init leaves it:
 * with ST7789_MV the panel is st7789_width() of 320 by st7789_height() of 172, and the gap lies in its rows.
 * The gap is the same from either side, so ST7789_MX and ST7789_MY move no picture off the glass.
 */
void st7789_orient(struct st7789 *lcd, uint8_t madctl);
uint32_t st7789_width(const struct st7789 *lcd);
uint32_t st7789_height(const struct st7789 *lcd);

/* Fill the rectangle [x0, x1] by [y0, y1] of the panel with one colour. */
void st7789_fill(const struct st7789 *lcd, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint16_t colour);

/*
 * Write the rectangle [x0, x1] by [y0, y1] of the panel pixel by pixel, row by row:
 * st7789_begin opens it, st7789_pixels sends n bytes of pixels, two a pixel, the high byte first,
 * as many times as it takes, and st7789_end closes it.
 * A buffer aligned to four bytes goes out fastest; see bus.h.
 */
void st7789_begin(const struct st7789 *lcd, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1);
void st7789_pixels(const uint8_t *bytes, uint32_t n);
void st7789_end(void);

#endif
