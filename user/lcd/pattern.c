/* The test pattern; see pattern.h. */

#include "pattern.h"

#include "st7789.h"

void pattern_draw(const struct st7789 *lcd)
{
    st7789_fill(lcd, 0, 0, LCD_W - 1, 79, RGB565(255, 0, 0));
    st7789_fill(lcd, 0, 80, LCD_W - 1, 159, RGB565(0, 255, 0));
    st7789_fill(lcd, 0, 160, LCD_W - 1, 239, RGB565(0, 0, 255));
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t level = i * 255 / 7;
        st7789_fill(lcd, 0, 240 + i * 10, LCD_W - 1, 249 + i * 10, RGB565(level, level, level));
    }
    const uint16_t white = RGB565(255, 255, 255);
    st7789_fill(lcd, 1, 1, LCD_W - 2, 1, white);
    st7789_fill(lcd, 1, LCD_H - 2, LCD_W - 2, LCD_H - 2, white);
    st7789_fill(lcd, 1, 1, 1, LCD_H - 2, white);
    st7789_fill(lcd, LCD_W - 2, 1, LCD_W - 2, LCD_H - 2, white);
}
