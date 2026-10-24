/*
 * The ST7789's commands, over bus.h; see st7789.h.
 * The waits are the datasheet's: 10 microseconds of reset at least,
 * 120 milliseconds after it before SLPOUT, and 120 after SLPOUT, which the supply needs to settle.
 */

#include "st7789.h"

#include "bus.h"

#define ST_SLPOUT 0x11u
#define ST_INVON  0x21u
#define ST_DISPON 0x29u
#define ST_CASET  0x2au
#define ST_RASET  0x2bu
#define ST_RAMWR  0x2cu
#define ST_GSCAN  0x45u
#define ST_MADCTL 0x36u
#define ST_COLMOD 0x3au

#define ST_MADCTL_DEFAULT 0x00u /* rows top to bottom, columns left to right, RGB */
#define ST_COLMOD_RGB565  0x55u

/* Pixels a fill sends at a time, a buffer of SPI2's. */
#define FILL_BYTES 64u

static void command(uint8_t c, const uint8_t *params, uint32_t n)
{
    lcd_bus_command(c);
    if (n > 0) {
        lcd_bus_data(params, n);
    }
    lcd_bus_end();
}

void st7789_init(struct st7789 *lcd)
{
    lcd_bus_reset(1);
    lcd_bus_sleep(10000);
    lcd_bus_reset(0);
    lcd_bus_sleep(120000);

    command(ST_SLPOUT, 0, 0);
    lcd_bus_sleep(120000);
    st7789_orient(lcd, ST_MADCTL_DEFAULT);
    const uint8_t colmod = ST_COLMOD_RGB565;
    command(ST_COLMOD, &colmod, 1);
    command(ST_INVON, 0, 0);
    command(ST_DISPON, 0, 0);
}

/* GSCAN's answer: the line's 16 bits, after a dummy clock. */
static uint32_t gscan(void)
{
    lcd_bus_command(ST_GSCAN);
    uint32_t line = lcd_bus_read() >> 15 & 0xffffu;
    lcd_bus_end();
    return line;
}

/*
 * The board's controller answers the first GSCAN after a RAMWR with the line it answered the GSCAN before it,
 * however long ago, and the next one with the line it is at; so it is asked twice, which costs 12 microseconds.
 */
uint32_t st7789_scanline(void)
{
    (void)gscan();
    return gscan();
}

/* Reads of the scan line st7789_sync makes at most, more than two frames' worth at the board's 10 microseconds a read. */
#define SYNC_READS 4096u

void st7789_sync(void)
{
    for (uint32_t i = 0; i < SYNC_READS; i++) {
        uint32_t line = st7789_scanline();
        if (line >= ST7789_SYNC_FIRST && line <= ST7789_SYNC_LAST) {
            return;
        }
    }
}

void st7789_orient(struct st7789 *lcd, uint8_t madctl)
{
    command(ST_MADCTL, &madctl, 1);
    lcd->madctl = madctl;
}

/* Whether rows and columns are exchanged, which moves the gap to the rows. */
static int exchanged(const struct st7789 *lcd)
{
    return (lcd->madctl & ST7789_MV) != 0;
}

uint32_t st7789_width(const struct st7789 *lcd)
{
    return exchanged(lcd) ? LCD_H : LCD_W;
}

uint32_t st7789_height(const struct st7789 *lcd)
{
    return exchanged(lcd) ? LCD_W : LCD_H;
}

static void window(const struct st7789 *lcd, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    if (exchanged(lcd)) {
        y0 += LCD_X_GAP;
        y1 += LCD_X_GAP;
    } else {
        x0 += LCD_X_GAP;
        x1 += LCD_X_GAP;
    }
    const uint8_t cols[4] = { (uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1 };
    const uint8_t rows[4] = { (uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1 };
    command(ST_CASET, cols, 4);
    command(ST_RASET, rows, 4);
}

void st7789_fill(const struct st7789 *lcd, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint16_t colour)
{
    window(lcd, x0, y0, x1, y1);
    _Alignas(4) uint8_t pixels[FILL_BYTES];
    for (uint32_t i = 0; i < FILL_BYTES; i += 2) {
        pixels[i] = (uint8_t)(colour >> 8);
        pixels[i + 1] = (uint8_t)colour;
    }
    uint32_t left = (x1 - x0 + 1) * (y1 - y0 + 1) * 2;
    lcd_bus_command(ST_RAMWR);
    while (left > 0) {
        uint32_t n = left < FILL_BYTES ? left : FILL_BYTES;
        lcd_bus_data(pixels, n);
        left -= n;
    }
    lcd_bus_end();
}

void st7789_begin(const struct st7789 *lcd, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    window(lcd, x0, y0, x1, y1);
    lcd_bus_command(ST_RAMWR);
}

void st7789_pixels(const uint8_t *bytes, uint32_t n)
{
    lcd_bus_data(bytes, n);
}

void st7789_end(void)
{
    lcd_bus_end();
}
