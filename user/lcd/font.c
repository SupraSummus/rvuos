/* The digits; see font.h. */

#include "font.h"

#include "st7789.h"

/* Each digit row by row from the top, each row's left dot its high bit. */
static const uint16_t digits[10] = {
    0x7b6f, 0x2c97, 0x73e7, 0x73cf, 0x5bc9, 0x79cf, 0x79ef, 0x7249, 0x7bef, 0x7bcf,
};

/* How many digits n has, 1 for 0. */
static uint32_t count(uint32_t n)
{
    uint32_t k = 1;
    while (n >= 10) {
        n /= 10;
        k++;
    }
    return k;
}

uint32_t font_row(uint32_t digit, uint32_t row)
{
    return digits[digit % 10] >> (FONT_W * (FONT_H - 1 - row)) & ((1u << FONT_W) - 1);
}

void font_number(const struct st7789 *lcd, uint32_t x, uint32_t y, uint32_t n, uint32_t scale, uint16_t colour)
{
    uint32_t k = count(n);
    for (uint32_t i = k; i-- > 0; n /= 10) {
        uint32_t x0 = x + i * (FONT_W + 1) * scale;
        for (uint32_t row = 0; row < FONT_H; row++) {
            for (uint32_t col = 0; col < FONT_W; col++) {
                if (font_row(n % 10, row) >> (FONT_W - 1 - col) & 1u) {
                    st7789_fill(lcd, x0 + col * scale, y + row * scale, x0 + col * scale + scale - 1, y + row * scale + scale - 1,
                                colour);
                }
            }
        }
    }
}
