/* The stress pattern; see stress.h. */

#include "stress.h"

#include "st7789.h"

#define GRADIENT_Y 0u
#define GRID_Y     80u
#define CHECKER_Y  160u
#define STRIPES_Y  240u
#define BAND_H     80u

#define SQUARE  8u
#define PITCH   12u
#define SQUARES_X ((LCD_W - SQUARE) / PITCH + 1)
#define SQUARES_Y ((BAND_H - SQUARE) / PITCH + 1)

static const uint16_t colours[] = {
    RGB565(255, 255, 255), RGB565(255, 0, 0),   RGB565(0, 255, 0),   RGB565(0, 0, 255),
    RGB565(255, 255, 0),   RGB565(0, 255, 255), RGB565(255, 0, 255),
};

static void put(uint8_t *row, uint32_t x, uint16_t c)
{
    row[2 * x] = (uint8_t)(c >> 8);
    row[2 * x + 1] = (uint8_t)c;
}

static void gradient(const struct st7789 *lcd)
{
    _Alignas(4) uint8_t row[LCD_W * 2];
    st7789_begin(lcd, 0, GRADIENT_Y, LCD_W - 1, GRADIENT_Y + BAND_H - 1);
    for (uint32_t y = 0; y < BAND_H; y++) {
        for (uint32_t x = 0; x < LCD_W; x++) {
            uint32_t r = x * 31 / (LCD_W - 1), g = y * 63 / (BAND_H - 1);
            put(row, x, (uint16_t)(r << 11 | g << 5 | (31 - r)));
        }
        st7789_pixels(row, sizeof(row));
    }
    st7789_end();
}

static void grid(const struct st7789 *lcd)
{
    st7789_fill(lcd, 0, GRID_Y, LCD_W - 1, GRID_Y + BAND_H - 1, RGB565(0, 0, 0));
    uint32_t x0 = (LCD_W - (SQUARES_X - 1) * PITCH - SQUARE) / 2;
    uint32_t y0 = GRID_Y + (BAND_H - (SQUARES_Y - 1) * PITCH - SQUARE) / 2;
    for (uint32_t j = 0; j < SQUARES_Y; j++) {
        for (uint32_t i = 0; i < SQUARES_X; i++) {
            uint32_t x = x0 + i * PITCH, y = y0 + j * PITCH;
            st7789_fill(lcd, x, y, x + SQUARE - 1, y + SQUARE - 1, colours[(i + j) % (sizeof(colours) / sizeof(colours[0]))]);
        }
    }
}

static void checker(const struct st7789 *lcd)
{
    _Alignas(4) uint8_t row[LCD_W * 2];
    st7789_begin(lcd, 0, CHECKER_Y, LCD_W - 1, CHECKER_Y + BAND_H - 1);
    for (uint32_t y = 0; y < BAND_H; y++) {
        for (uint32_t x = 0; x < LCD_W; x++) {
            uint32_t odd = (x + y) & 1u;
            put(row, x, x < LCD_W / 2 ? (odd ? 0x5555u : 0xaaaau) : (odd ? 0x0000u : 0xffffu));
        }
        st7789_pixels(row, sizeof(row));
    }
    st7789_end();
}

static void stripes(const struct st7789 *lcd)
{
    _Alignas(4) uint8_t row[LCD_W * 2];
    for (uint32_t x = 0; x < LCD_W; x++) {
        put(row, x, colours[(x / 4) % 4]);
    }
    st7789_begin(lcd, 0, STRIPES_Y, LCD_W - 1, STRIPES_Y + BAND_H - 1);
    for (uint32_t y = 0; y < BAND_H; y++) {
        st7789_pixels(row, sizeof(row));
    }
    st7789_end();
}

void stress_draw(const struct st7789 *lcd)
{
    gradient(lcd);
    grid(lcd);
    checker(lcd);
    stripes(lcd);
}
