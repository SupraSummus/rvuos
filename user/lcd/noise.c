/* Snow; see noise.h. */

#include "noise.h"

static void picture(const struct st7789 *lcd, uint32_t (*random)(void))
{
    _Alignas(4) uint8_t row[LCD_W * 2];
    uint32_t word = 0;
    st7789_begin(lcd, 0, 0, LCD_W - 1, LCD_H - 1);
    for (uint32_t y = 0; y < LCD_H; y++) {
        for (uint32_t x = 0; x < LCD_W; x++) {
            if ((y * LCD_W + x) % 8 == 0) {
                word = random();
            }
            uint32_t grey = 17u * (word & 0xfu);
            uint16_t c = RGB565(grey, grey, grey);
            row[2 * x] = (uint8_t)(c >> 8);
            row[2 * x + 1] = (uint8_t)c;
            word >>= 4;
        }
        st7789_pixels(row, sizeof(row));
    }
    st7789_end();
}

void noise_draw(const struct st7789 *lcd, uint32_t (*random)(void), uint32_t pictures)
{
    for (uint32_t i = 0; pictures == 0 || i < pictures; i++) {
        picture(lcd, random);
    }
}
