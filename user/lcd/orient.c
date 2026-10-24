/* The orientation's pattern; see orient.h. */

#include "orient.h"

#include "bus.h"
#include "font.h"
#include "st7789.h"

#define BLACK RGB565(0, 0, 0)
#define WHITE RGB565(255, 255, 255)
#define RED   RGB565(255, 0, 0)
#define GREEN RGB565(0, 255, 0)
#define BLUE  RGB565(0, 0, 255)

const uint8_t orient_settings[ORIENT_SETTINGS] = {
    0, ST7789_MX, ST7789_MY, ST7789_MY | ST7789_MX,
    ST7789_MV, ST7789_MV | ST7789_MX, ST7789_MV | ST7789_MY, ST7789_MV | ST7789_MY | ST7789_MX,
    ST7789_BGR,
};

static void square(const struct st7789 *lcd, uint32_t x, uint32_t y, uint16_t colour)
{
    st7789_fill(lcd, x, y, x + ORIENT_SQUARE - 1, y + ORIENT_SQUARE - 1, colour);
}

void orient_draw(struct st7789 *lcd, uint8_t madctl)
{
    st7789_orient(lcd, madctl);
    uint32_t w = st7789_width(lcd), h = st7789_height(lcd);
    st7789_fill(lcd, 0, 0, w - 1, h - 1, BLACK);
    st7789_fill(lcd, 0, 0, w - 1, 0, WHITE);
    st7789_fill(lcd, 0, h - 1, w - 1, h - 1, WHITE);
    st7789_fill(lcd, 0, 0, 0, h - 1, WHITE);
    st7789_fill(lcd, w - 1, 0, w - 1, h - 1, WHITE);
    square(lcd, ORIENT_INSET, ORIENT_INSET, RED);
    square(lcd, w - ORIENT_INSET - ORIENT_SQUARE, ORIENT_INSET, GREEN);
    square(lcd, ORIENT_INSET, h - ORIENT_INSET - ORIENT_SQUARE, BLUE);
    font_number(lcd, ORIENT_INSET + ORIENT_SQUARE + ORIENT_SCALE * 2, ORIENT_INSET, madctl, ORIENT_SCALE, WHITE);
}

void orient_cycle(struct st7789 *lcd)
{
    for (uint32_t i = 0; i < ORIENT_SETTINGS; i++) {
        orient_draw(lcd, orient_settings[i]);
        lcd_bus_sleep(ORIENT_HOLD_US);
    }
    orient_draw(lcd, 0);
}
