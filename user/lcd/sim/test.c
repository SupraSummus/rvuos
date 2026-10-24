/*
 * The LCD's test on the host, which make check runs:
 * app.c against the model of the board's LCD, which must meet no error and show the test pattern where pattern.h says,
 * the frame on the glass's second pixel from each edge and the bands' colours inside it,
 * and the model itself, which must refuse what the controller would not take, so that its silence means something,
 * and must hide what the glass's round corners hide, and show a picture torn only when it is not drawn in sync;
 * and the veins scene, whose light must stay even, refresh to refresh, as veins.h says.
 * A pixel the corners hide is held to what the program drew in the controller's memory instead.
 * The model's own facts are the board's or the datasheet's, see panel.c; this holds the program to them, not them to the board.
 */

#include <stdio.h>

#include "../app.h"
#include "../bus.h"
#include "../st7789.h"
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

/* What the program drew at x, y of the glass, whether the glass shows it or its corners hide it. */
static void expect_drawn(uint32_t x, uint32_t y, uint32_t want, const char *what)
{
    int on_glass;
    uint32_t got = panel_memory(x + LCD_X_GAP, y, &on_glass);
    if (got != want || !on_glass) {
        printf("lcd-test: %s: the memory under the glass at %u, %u holds %06x, not %06x: FAIL\n", what, x, y, got, want);
        failures++;
    }
}

/* That the glass shows percent of the light of what the memory holds at x, y, each colour's part rounded down. */
static void expect_dimmed(uint32_t x, uint32_t y, uint32_t percent, const char *what)
{
    int on_glass;
    uint32_t held = panel_memory(x + LCD_X_GAP, y, &on_glass), want = 0;
    for (uint32_t shift = 0; shift < 24; shift += 8) {
        want |= (((held >> shift) & 0xffu) * percent / 100) << shift;
    }
    expect_pixel(x, y, want, what);
}

/* A frame and a little more, for the refresh to bring what was drawn onto the glass. */
static void settle(void)
{
    lcd_bus_sleep(ST7789_FRAME_US + 100u);
}

/*
 * The refreshes seen torn, each time the refresh has passed the glass's last row: the glass's middle column
 * not all of one colour, which, as each picture tears() draws is of one colour, is a tear.
 */
static uint64_t refreshes_seen;
static uint32_t torn;

static void watch_tears(uint64_t now_ns)
{
    (void)now_ns;
    if (panel_refreshes() == refreshes_seen) {
        return;
    }
    refreshes_seen = panel_refreshes();
    for (uint32_t y = 1; y < GLASS_H; y++) {
        if (panel_shown(GLASS_W / 2, y) != panel_shown(GLASS_W / 2, 0)) {
            torn++;
            return;
        }
    }
}

/*
 * The refreshes torn of PICTURES whole pictures, red and blue in turn, at the bus's clock of mhz,
 * each started in st7789_sync's window, with first 0, or once the refresh is at a line from first to last.
 */
#define PICTURES 8u

static uint32_t tears(uint32_t mhz, uint32_t first, uint32_t last)
{
    struct st7789 lcd;
    panel_bus_mhz(mhz);
    lcd_bus_init();
    st7789_init(&lcd);
    st7789_fill(&lcd, 0, 0, LCD_W - 1, LCD_H - 1, RGB565(0, 0, 255));
    lcd_bus_backlight(LCD_BACKLIGHT_FULL);
    settle();
    refreshes_seen = panel_refreshes();
    torn = 0;
    panel_on_time(watch_tears);
    for (uint32_t i = 0; i < PICTURES; i++) {
        if (first == 0) {
            st7789_sync();
        } else {
            uint32_t line = st7789_scanline();
            while (line < first || line > last) {
                line = st7789_scanline();
            }
        }
        st7789_fill(&lcd, 0, 0, LCD_W - 1, LCD_H - 1, i % 2 ? RGB565(0, 0, 255) : RGB565(255, 0, 0));
    }
    settle();
    panel_on_time(0);
    panel_bus_mhz(40);
    return torn;
}

/*
 * The veins scene's light: the glass's mean, in the sum of its three colours, at each refresh from LIGHT_FROM on,
 * counted from the scene's start, when the first network has grown out of the point it starts in,
 * and the most it moved from one refresh to the next.
 */
#define LIGHT_PICTURES 150u
#define LIGHT_FROM     75u

static uint64_t light_start, light_refreshes;
static uint32_t light_last, light_least, light_most, light_moved;

static void watch_light(uint64_t now_ns)
{
    (void)now_ns;
    if (panel_refreshes() == light_refreshes) {
        return;
    }
    light_refreshes = panel_refreshes() - light_start;
    if (light_refreshes < LIGHT_FROM) {
        return;
    }
    uint64_t sum = 0;
    for (uint32_t y = 0; y < GLASS_H; y++) {
        for (uint32_t x = 0; x < GLASS_W; x++) {
            uint32_t c = panel_shown(x, y);
            sum += (c >> 16) + (c >> 8 & 0xffu) + (c & 0xffu);
        }
    }
    uint32_t light = (uint32_t)(sum / (GLASS_W * GLASS_H));
    if (light_refreshes > LIGHT_FROM) {
        uint32_t moved = light > light_last ? light - light_last : light_last - light;
        light_moved = moved > light_moved ? moved : light_moved;
    }
    light_least = light < light_least ? light : light_least;
    light_most = light > light_most ? light : light_most;
    light_last = light;
}

/* The host's random words for the veins scene, Marsaglia's xorshift32, as the simulator's. */
static uint32_t xorshift(void)
{
    static uint32_t x = 2463534242u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
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
    lcd_bus_backlight(LCD_BACKLIGHT_FULL);
    settle();
}

int main(void)
{
    const struct app_options options = APP_OPTIONS_DEFAULT;
    struct st7789 lcd;
    lcd_bus_init();
    app_run(&lcd, APP_PATTERN, &options);
    settle();
    expect_errors(0, "the program");
    expect_drawn(0, 0, RED, "the red band, outside the frame");
    expect_drawn(1, 1, WHITE, "the frame's top left");
    expect_drawn(170, 318, WHITE, "the frame's bottom right");
    expect_pixel(1, 30, WHITE, "the frame's left side, past the top left corner");
    expect_pixel(0, 0, BLACK, "the top left corner, which the glass hides");
    expect_pixel(15, 0, BLACK, "the glass's top row, hidden up to its sixteenth pixel");
    expect_dimmed(3, 10, 50, "the top left corner's edge, half shown");
    expect_dimmed(LCD_W - 1 - 3, LCD_H - 1 - 10, 50, "the bottom right corner's edge, as the top left's mirrored");
    expect_dimmed(0, 19, 90, "the last row the top left corner dims");
    expect_dimmed(0, 20, 100, "the first row past the top left corner");
    expect_pixel(171, 100, GREEN, "the green band, outside the frame");
    expect_pixel(86, 40, RED, "the red band");
    expect_pixel(86, 120, GREEN, "the green band");
    expect_pixel(86, 200, BLUE, "the blue band");
    expect_pixel(86, 245, BLACK, "the darkest grey");
    expect_pixel(86, 315, WHITE, "the lightest grey");

    lcd_bus_init();
    app_run(&lcd, APP_STRESS, &options);
    settle();
    expect_errors(0, "the stress pattern");
    expect_drawn(0, 0, 0x0000ffu, "the gradient's top left, no red, no green, all blue");
    expect_pixel(171, 79, 0xffff00u, "the gradient's bottom right, all red and green, no blue");
    expect_pixel(4, 83, WHITE, "the grid's first square");
    expect_pixel(3, 83, BLACK, "beside the grid's first square");
    expect_pixel(0, 160, 0xad5552u, "the checkerboard's 0xaaaa");
    expect_pixel(1, 160, 0x52aaadu, "the checkerboard's 0x5555");
    expect_pixel(86, 160, WHITE, "the checkerboard's white");
    expect_pixel(87, 160, BLACK, "the checkerboard's black");
    expect_drawn(3, 319, WHITE, "the first stripe");
    expect_drawn(4, 319, RED, "the second stripe");
    expect_pixel(83, 319, WHITE, "a stripe on the glass's last row");

    struct app_options orient = APP_OPTIONS_DEFAULT;
    orient.madctl = 0;
    lcd_bus_init();
    app_run(&lcd, APP_ORIENT, &orient);
    settle();
    expect_errors(0, "the orientation as st7789_init leaves it");
    expect_pixel(40, 40, RED, "the orientation's red square, top left");
    expect_pixel(LCD_W - 40, 40, GREEN, "the orientation's green square, top right");
    expect_pixel(40, LCD_H - 40, BLUE, "the orientation's blue square, bottom left");
    expect_pixel(LCD_W - 1, 160, WHITE, "the orientation's frame on the glass's last column");
    orient.madctl = ST7789_MV | ST7789_MX;
    lcd_bus_init();
    app_run(&lcd, APP_ORIENT, &orient);
    settle();
    expect_errors(0, "the orientation turned, MV and MX");
    expect_pixel(LCD_W - 40, 40, RED, "MV and MX: the picture's top left at the glass's top right");
    expect_pixel(LCD_W - 40, LCD_H - 40, GREEN, "MV and MX: the picture's top right at the glass's bottom right");
    expect_pixel(40, 40, BLUE, "MV and MX: the picture's bottom left at the glass's top left");
    expect_pixel(LCD_W - 1, 160, WHITE, "MV and MX: the frame's top on the glass's last column, the gap in the picture's rows");
    expect_pixel(86, LCD_H - 1, WHITE, "MV and MX: the frame's right on the glass's last row");
    orient.madctl = ST7789_BGR;
    lcd_bus_init();
    app_run(&lcd, APP_ORIENT, &orient);
    settle();
    expect_pixel(40, 40, BLUE, "BGR: red written, blue shown");
    orient.madctl = ORIENT_ALL;
    lcd_bus_init();
    app_run(&lcd, APP_ORIENT, &orient);
    settle();
    expect_errors(0, "each orientation in turn");
    expect_pixel(40, 40, RED, "each orientation in turn, then MADCTL 0 again");

    /*
     * In sync, no refresh is torn; started where st7789.h says a picture of that speed tears, some are,
     * which shows the check sees a tear: at 80 MHz, early, where the picture catches the refresh up,
     * and at 40 MHz, late, where the refresh comes round and catches the picture up.
     */
    static const struct {
        uint32_t mhz, first, last;
    } bad[] = { { 40, 300, 310 }, { 80, 20, 30 } };
    for (uint32_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        uint32_t synced = tears(bad[i].mhz, 0, 0), control = tears(bad[i].mhz, bad[i].first, bad[i].last);
        expect_errors(0, "the pictures in turn");
        if (synced != 0) {
            printf("lcd-test: %u refreshes torn of pictures drawn in sync at %u MHz: FAIL\n", synced, bad[i].mhz);
            failures++;
        }
        if (control == 0) {
            printf("lcd-test: no refresh torn of pictures started at lines %u to %u at %u MHz, so the check sees nothing: FAIL\n",
                   bad[i].first, bad[i].last, bad[i].mhz);
            failures++;
        }
    }

    /*
     * The veins scene, at the bus's 80 MHz: no error, and once its network has grown its light stays even, nothing flashing,
     * its mean neither dark nor burnt out and never moving by more than a twentieth from one refresh to the next.
     */
    static _Alignas(4) uint8_t veins_memory[VEINS_MEMORY];
    struct app_options veins = APP_OPTIONS_DEFAULT;
    veins.random = xorshift;
    veins.memory = veins_memory;
    veins.pictures = LIGHT_PICTURES;
    panel_bus_mhz(80);
    lcd_bus_init();
    light_least = UINT32_MAX;
    light_start = panel_refreshes();
    panel_on_time(watch_light);
    app_run(&lcd, APP_VEINS, &veins);
    panel_on_time(0);
    panel_bus_mhz(40);
    expect_errors(0, "the veins scene");
    printf("lcd-test: the veins scene's light from refresh %u to %llu: %u to %u, moving by %u at most\n", LIGHT_FROM,
           (unsigned long long)light_refreshes, light_least, light_most, light_moved);
    if (light_refreshes < LIGHT_FROM + 30u || light_least < 10u || light_most > 200u || light_moved * 20u > light_least) {
        printf("lcd-test: the veins scene's light is not even: FAIL\n");
        failures++;
    }

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
