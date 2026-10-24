/*
 * The model of the board's LCD; see panel.h.
 *
 * Each fact the model rests on is the board's, seen on it, or the datasheet's alone, which the board has yet to show.
 * The board showed, through a camera, on 2026-10-24, with the test pattern of pattern.h:
 *   - the glass shows columns 34 to 205 of the controller's memory, all 320 rows, with MADCTL at zero;
 *   - the panel shows the controller's colours inverted, so it shows them as written only with INVON;
 *   - with MADCTL's RGB bit clear and COLMOD 0x55, red, green and blue land where written, the high byte first.
 * The datasheet's alone:
 *   - MADCTL's MY, MX and MV, its BGR bit, and that its ML and MH change only the order of refresh;
 *   - RAMWRC, and that writes past the window's end wrap to its start;
 *   - the waits: a reset of 10 microseconds at least, 5 milliseconds after a reset, SLPOUT or SLPIN before another command,
 *     120 milliseconds after a reset or SLPIN before SLPOUT, and after SLPOUT before SLPIN;
 *   - the state after a reset: asleep, the display off, no inversion, MADCTL zero, COLMOD 0x66, the window the whole memory,
 *     and the memory as it was, which at power on is noise;
 *   - that the glass is dark while the controller sleeps or its display is off, as this panel is black when not driven.
 * A fact the board shows to be otherwise moves the model, and a datasheet's fact it shows moves up to the board's.
 *
 * Time passes on the bus at the board's clock, 40 MHz unless panel_bus_mhz says otherwise,
 * with no gap between bytes, which the board's CPU leaves;
 * so the model's time for a picture is the least the board takes.
 */

#include "panel.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../bus.h"

#define GLASS_X0 34u /* the first column of memory the glass shows */

#define US         1000u
#define MS         1000000u

#define RESET_PULSE_NS (10u * US)
#define READY_NS       (5u * MS)
#define SETTLE_NS      (120u * MS)

#define MADCTL_MY  0x80u
#define MADCTL_MX  0x40u
#define MADCTL_MV  0x20u
#define MADCTL_BGR 0x08u

#define COLMOD_RGB565 0x55u

enum { STREAM = 0xff }; /* a command whose data is pixels, as many as come */

struct command {
    uint8_t code;
    uint8_t params; /* or STREAM */
    const char *name;
};

static const struct command commands[] = {
    { 0x00, 0, "NOP" },     { 0x01, 0, "SWRESET" }, { 0x10, 0, "SLPIN" },  { 0x11, 0, "SLPOUT" },
    { 0x13, 0, "NORON" },   { 0x20, 0, "INVOFF" },  { 0x21, 0, "INVON" },  { 0x28, 0, "DISPOFF" },
    { 0x29, 0, "DISPON" },  { 0x2a, 4, "CASET" },   { 0x2b, 4, "RASET" },  { 0x2c, STREAM, "RAMWR" },
    { 0x36, 1, "MADCTL" },  { 0x3a, 1, "COLMOD" },  { 0x3c, STREAM, "RAMWRC" },
};

static struct {
    uint16_t memory[MEMORY_H][MEMORY_W];
    uint64_t now_ns;
    uint64_t bytes;
    uint32_t errors;
    void (*hook)(uint64_t now_ns);
    uint32_t mhz;

    int selected;
    int in_reset;
    uint64_t reset_since;
    uint64_t ready_at;  /* no command before this */
    uint64_t slpout_at; /* no SLPOUT before this */
    uint64_t slpin_at;  /* no SLPIN before this */

    int asleep;
    int display_on;
    int inverted;
    int backlight;
    uint8_t madctl;
    uint8_t colmod;
    uint32_t xs, xe, ys, ye;

    const struct command *command; /* the one being sent, or 0 */
    uint8_t params[4];
    uint32_t nparams;
    uint32_t x, y;   /* where the next pixel goes, in the window's terms */
    int half;        /* a pixel's high byte came, and its low byte has not */
    uint8_t high;
    int pixel_error; /* the command's pixels were refused, once */
} p;

static void error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "panel: at %.3f ms: ", (double)p.now_ns / MS);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    p.errors++;
}

static void pass(uint64_t ns)
{
    p.now_ns += ns;
    if (p.hook != 0) {
        p.hook(p.now_ns);
    }
}

/* The state after a reset, of the pin or SWRESET; the memory keeps what it held. */
static void reset(void)
{
    p.asleep = 1;
    p.display_on = 0;
    p.inverted = 0;
    p.madctl = 0;
    p.colmod = 0x66;
    p.xs = 0;
    p.xe = MEMORY_W - 1;
    p.ys = 0;
    p.ye = MEMORY_H - 1;
    p.ready_at = p.now_ns + READY_NS;
    p.slpout_at = p.now_ns + SETTLE_NS;
}

static uint32_t logical_w(void)
{
    return p.madctl & MADCTL_MV ? MEMORY_H : MEMORY_W;
}

static uint32_t logical_h(void)
{
    return p.madctl & MADCTL_MV ? MEMORY_W : MEMORY_H;
}

/* Write a pixel where the window's pointer is, through MADCTL, and move the pointer on, wrapping at the window's end. */
static void put_pixel(uint16_t colour)
{
    uint32_t a = p.madctl & MADCTL_MX ? logical_w() - 1 - p.x : p.x;
    uint32_t b = p.madctl & MADCTL_MY ? logical_h() - 1 - p.y : p.y;
    if (p.madctl & MADCTL_MV) {
        p.memory[a][b] = colour;
    } else {
        p.memory[b][a] = colour;
    }
    if (++p.x > p.xe) {
        p.x = p.xs;
        if (++p.y > p.ye) {
            p.y = p.ys;
        }
    }
}

static int window_fits(void)
{
    return p.xs <= p.xe && p.ys <= p.ye && p.xe < logical_w() && p.ye < logical_h();
}

/* Run a command whose parameters have all come, or which takes none. */
static void run(void)
{
    const uint8_t *q = p.params;
    switch (p.command->code) {
    case 0x01: /* SWRESET */
        reset();
        break;
    case 0x10: /* SLPIN */
        if (p.now_ns < p.slpin_at) {
            error("SLPIN within 120 ms of SLPOUT");
        }
        p.asleep = 1;
        p.ready_at = p.now_ns + READY_NS;
        p.slpout_at = p.now_ns + SETTLE_NS;
        break;
    case 0x11: /* SLPOUT */
        if (p.now_ns < p.slpout_at) {
            error("SLPOUT within 120 ms of a reset or SLPIN");
        }
        p.asleep = 0;
        p.ready_at = p.now_ns + READY_NS;
        p.slpin_at = p.now_ns + SETTLE_NS;
        break;
    case 0x20: /* INVOFF */
    case 0x21: /* INVON */
        p.inverted = p.command->code == 0x21;
        break;
    case 0x28: /* DISPOFF */
    case 0x29: /* DISPON */
        p.display_on = p.command->code == 0x29;
        break;
    case 0x2a: /* CASET */
        p.xs = (uint32_t)q[0] << 8 | q[1];
        p.xe = (uint32_t)q[2] << 8 | q[3];
        break;
    case 0x2b: /* RASET */
        p.ys = (uint32_t)q[0] << 8 | q[1];
        p.ye = (uint32_t)q[2] << 8 | q[3];
        break;
    case 0x2c: /* RAMWR */
    case 0x3c: /* RAMWRC */
        if (!window_fits()) {
            error("%s into the window [%u, %u] by [%u, %u], which does not fit the memory of %u by %u as MADCTL 0x%02x lays it",
                  p.command->name, p.xs, p.xe, p.ys, p.ye, logical_w(), logical_h(), p.madctl);
            p.pixel_error = 1;
        }
        if (p.command->code == 0x2c) {
            p.x = p.xs;
            p.y = p.ys;
        }
        break;
    case 0x36: /* MADCTL */
        p.madctl = q[0];
        break;
    case 0x3a: /* COLMOD */
        p.colmod = q[0];
        if (p.colmod != COLMOD_RGB565) {
            error("COLMOD 0x%02x, which is not modelled; only 0x%02x, RGB565, is", p.colmod, COLMOD_RGB565);
        }
        break;
    default: /* NOP, NORON */
        break;
    }
}

/* The command being sent has ended, by the select or the next command: it must have had all its parameters. */
static void finish(void)
{
    if (p.command == 0) {
        return;
    }
    if (p.command->params != STREAM && p.nparams < p.command->params) {
        error("%s ended after %u of its %u parameters", p.command->name, p.nparams, p.command->params);
    }
    if (p.command->params == STREAM && p.half) {
        error("%s ended within a pixel, after its high byte", p.command->name);
    }
    p.command = 0;
}

void lcd_bus_init(void)
{
    uint32_t mhz = p.mhz != 0 ? p.mhz : 40;
    memset(&p, 0, sizeof(p));
    p.mhz = mhz;
    /* The memory at power on: noise, which shows whatever a program leaves unwritten. */
    uint32_t seed = 0x2545f491u;
    for (uint32_t y = 0; y < MEMORY_H; y++) {
        for (uint32_t x = 0; x < MEMORY_W; x++) {
            seed = seed * 1664525u + 1013904223u;
            p.memory[y][x] = (uint16_t)(seed >> 16);
        }
    }
    reset();
}

void lcd_bus_command(uint8_t code)
{
    finish();
    p.selected = 1;
    p.bytes++;
    pass(8000u / p.mhz);
    if (p.in_reset) {
        error("command 0x%02x while the controller is held in reset", code);
    }
    p.command = 0;
    for (uint32_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        if (commands[i].code == code) {
            p.command = &commands[i];
        }
    }
    if (p.command == 0) {
        error("command 0x%02x, which is not modelled", code);
        return;
    }
    if (p.now_ns < p.ready_at) {
        error("%s %.3f ms before the controller takes commands, 5 ms after a reset, SLPOUT or SLPIN",
              p.command->name, (double)(p.ready_at - p.now_ns) / MS);
    }
    p.nparams = 0;
    p.half = 0;
    p.pixel_error = 0;
    if (p.command->params == 0 || p.command->params == STREAM) {
        run();
    }
}

void lcd_bus_data(const uint8_t *bytes, uint32_t n)
{
    p.bytes += n;
    pass((uint64_t)n * 8000u / p.mhz);
    if (!p.selected || p.command == 0) {
        error("%u bytes of data with no command they belong to", n);
        return;
    }
    if (p.command->params != STREAM) {
        if (p.nparams + n > p.command->params) {
            error("%s given %u parameters, which takes %u", p.command->name, p.nparams + n, p.command->params);
            return;
        }
        memcpy(p.params + p.nparams, bytes, n);
        p.nparams += n;
        if (p.nparams == p.command->params) {
            run();
        }
        return;
    }
    if (p.colmod != COLMOD_RGB565 && !p.pixel_error) {
        error("pixels in COLMOD 0x%02x, which is not modelled; only 0x%02x, RGB565, is", p.colmod, COLMOD_RGB565);
        p.pixel_error = 1;
    }
    if (p.pixel_error) {
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (!p.half) {
            p.high = bytes[i];
            p.half = 1;
        } else {
            put_pixel((uint16_t)(p.high << 8 | bytes[i]));
            p.half = 0;
        }
    }
}

void lcd_bus_end(void)
{
    finish();
    p.selected = 0;
}

void lcd_bus_reset(int low)
{
    if (low && !p.in_reset) {
        p.in_reset = 1;
        p.reset_since = p.now_ns;
    } else if (!low && p.in_reset) {
        p.in_reset = 0;
        if (p.now_ns - p.reset_since < RESET_PULSE_NS) {
            error("a reset of %.3f us, shorter than the 10 us the controller takes as one", (double)(p.now_ns - p.reset_since) / US);
        } else {
            reset();
        }
    }
}

void lcd_bus_backlight(int on)
{
    p.backlight = on;
}

void lcd_bus_sleep(uint32_t us)
{
    pass((uint64_t)us * US);
}

/* RGB565 as 0xRRGGBB, each part's top bits repeated into its low ones. */
static uint32_t rgb888(uint16_t c)
{
    uint32_t r = c >> 11, g = (c >> 5) & 0x3fu, b = c & 0x1fu;
    return (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
}

static uint32_t shown(uint16_t c)
{
    uint32_t rgb = rgb888(c);
    if (p.madctl & MADCTL_BGR) {
        rgb = (rgb & 0xff00u) | (rgb >> 16) | (rgb & 0xffu) << 16;
    }
    return p.inverted ? rgb : rgb ^ 0xffffffu;
}

uint32_t panel_shown(uint32_t x, uint32_t y)
{
    if (!p.backlight || p.in_reset || p.asleep || !p.display_on) {
        return 0;
    }
    return shown(p.memory[y][x + GLASS_X0]);
}

uint32_t panel_memory(uint32_t x, uint32_t y, int *on_glass)
{
    *on_glass = x >= GLASS_X0 && x < GLASS_X0 + GLASS_W && y < GLASS_H;
    return shown(p.memory[y][x]);
}

uint32_t panel_errors(void)
{
    return p.errors;
}

uint64_t panel_now_ns(void)
{
    return p.now_ns;
}

uint64_t panel_bytes(void)
{
    return p.bytes;
}

void panel_on_time(void (*hook)(uint64_t now_ns))
{
    p.hook = hook;
}

void panel_bus_mhz(uint32_t mhz)
{
    p.mhz = mhz;
}
