/*
 * The LCD's test on the host, which make check runs:
 * app.c against the model of the board's LCD, which must meet no error and show the test pattern where pattern.h says,
 * the frame on the glass's second pixel from each edge and the bands' colours inside it,
 * and the model itself, which must refuse what the controller would not take, so that its silence means something.
 * The model's own facts are the board's or the datasheet's, see panel.c; this holds the program to them, not them to the board.
 */

#include <stdio.h>

#include "../app.h"
#include "../bus.h"
#include "panel.h"

#define RED   0xff0000u
#define GREEN 0x00ff00u
#define BLUE  0x0000ffu
#define BLACK 0x000000u
#define WHITE 0xffffffu

static int failures;

static void expect_pixel(uint32_t x, uint32_t y, uint32_t want, const char *what)
{
    uint32_t got = panel_shown(x, y);
    if (got != want) {
        printf("lcd-test: %s: the glass at %u, %u shows %06x, not %06x: FAIL\n", what, x, y, got, want);
        failures++;
    }
}

static void expect_errors(int some, const char *what)
{
    if ((panel_errors() != 0) != some) {
        printf("lcd-test: %s: the model met %u errors: FAIL\n", what, panel_errors());
        failures++;
    }
}

/* The controller woken as st7789.c wakes it, but for what skip names, then a red screen. */
static void wake_without(uint8_t skip)
{
    static const uint8_t madctl = 0x00, colmod = 0x55, red[2] = { 0xf8, 0x00 };
    static const uint8_t window[2][4] = { { 0, 34, 0, 205 }, { 0, 0, 1, 63 } };
    lcd_bus_init();
    lcd_bus_reset(1);
    lcd_bus_sleep(10000);
    lcd_bus_reset(0);
    lcd_bus_sleep(120000);
    lcd_bus_command(0x11);
    lcd_bus_end();
    lcd_bus_sleep(skip == 0xff ? 1000 : 120000); /* 0xff: too short a wait after SLPOUT */
    if (skip != 0x36) {
        lcd_bus_command(0x36);
        lcd_bus_data(&madctl, 1);
        lcd_bus_end();
    }
    if (skip != 0x3a) {
        lcd_bus_command(0x3a);
        lcd_bus_data(&colmod, 1);
        lcd_bus_end();
    }
    if (skip != 0x21) {
        lcd_bus_command(0x21);
        lcd_bus_end();
    }
    lcd_bus_command(0x29);
    lcd_bus_end();
    lcd_bus_command(0x2a);
    lcd_bus_data(window[0], 4);
    lcd_bus_end();
    lcd_bus_command(0x2b);
    lcd_bus_data(window[1], 4);
    lcd_bus_end();
    lcd_bus_command(0x2c);
    for (uint32_t i = 0; i < 172u * 320u; i++) {
        lcd_bus_data(red, 2);
    }
    lcd_bus_end();
    lcd_bus_backlight(1);
}

int main(void)
{
    lcd_bus_init();
    app_run(APP_PATTERN);
    expect_errors(0, "the program");
    expect_pixel(0, 0, RED, "the red band, outside the frame");
    expect_pixel(1, 1, WHITE, "the frame's top left");
    expect_pixel(170, 318, WHITE, "the frame's bottom right");
    expect_pixel(171, 100, GREEN, "the green band, outside the frame");
    expect_pixel(86, 40, RED, "the red band");
    expect_pixel(86, 120, GREEN, "the green band");
    expect_pixel(86, 200, BLUE, "the blue band");
    expect_pixel(86, 245, BLACK, "the darkest grey");
    expect_pixel(86, 315, WHITE, "the lightest grey");

    lcd_bus_init();
    app_run(APP_STRESS);
    expect_errors(0, "the stress pattern");
    expect_pixel(0, 0, 0x0000ffu, "the gradient's top left, no red, no green, all blue");
    expect_pixel(171, 79, 0xffff00u, "the gradient's bottom right, all red and green, no blue");
    expect_pixel(4, 83, WHITE, "the grid's first square");
    expect_pixel(3, 83, BLACK, "beside the grid's first square");
    expect_pixel(0, 160, 0xad5552u, "the checkerboard's 0xaaaa");
    expect_pixel(1, 160, 0x52aaadu, "the checkerboard's 0x5555");
    expect_pixel(86, 160, WHITE, "the checkerboard's white");
    expect_pixel(87, 160, BLACK, "the checkerboard's black");
    expect_pixel(3, 319, WHITE, "the first stripe");
    expect_pixel(4, 319, RED, "the second stripe");

    wake_without(0);
    expect_errors(0, "the model's own wake");
    expect_pixel(86, 160, RED, "the model's own wake");
    wake_without(0x21);
    expect_errors(0, "no INVON");
    expect_pixel(86, 160, 0x00ffffu, "no INVON, which this panel shows inverted");
    printf("lcd-test: the model must refuse the two wakes below, so its errors follow\n");
    fflush(stdout);
    wake_without(0x3a);
    expect_errors(1, "no COLMOD, so the controller's 18 bits a pixel");
    wake_without(0xff);
    expect_errors(1, "a command 1 ms after SLPOUT");

    if (failures != 0) {
        printf("lcd-test: %d failed\n", failures);
        return 1;
    }
    printf("lcd-test: PASS\n");
    return 0;
}
