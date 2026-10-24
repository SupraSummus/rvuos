/*
 * LCD: a root task that drives the LCD of Waveshare's ESP32-C5-LCD-1.47,
 * an ST7789 of 172 by 320 pixels on SPI2, and shows what app.c shows on it.
 *
 * The program is layered so that all but the bottom runs on a host as well:
 * app.c draws, pattern.c among it, through st7789.c, which speaks the controller's commands over bus.h;
 * on the board the bus is spi2.c, and on the host the model of the controller in sim/, which renders what it is sent.
 * This file is the board's alone: it maps the devices, keeps the timer bus.h's waits sleep on, and starts the rest.
 * It writes on the console what app.c took, its waits among them, in the core's cycles,
 * which the simulator's least time, the bus's alone, compares with.
 *
 * Its input region holds a configuration, words of key=value:
 * scene= names app.c's scene, pattern unless given, mhz= the bus's clock, 40 or 80, and drive= its pins' drive, 0 to 3,
 * see spi2.h, madctl= the orient scene's MADCTL in decimal, such as madctl=96, each of orient.h's in turn unless given,
 * backlight= its level, 0 to 255, see bus.h, 255 unless given,
 * and pictures= how many the noise scene draws, see noise.h, 0 for no end, which never halts.
 * The anim scene's report, see anim.h, follows the drawing on the console.
 * The noise scene writes the random number generator's RNG_CFG before it draws and after, its top byte the ring oscillator's samples.
 * What it does not find it leaves at the default.
 *
 * The probe scene leaves the screen black and takes lines from the console, for user/lcd/tools/probe.py:
 * "x y" lights the panel's pixel at x, y and puts out the one lit before, "off" puts it out,
 * "mark x y" lights a pixel for good, which neither "off" nor the next "x y" puts out, a mark a camera finds the panel by,
 * and "end" halts the machine.
 * It answers each with a line, "lcd: probe x y", "lcd: probe off", "lcd: probe mark x y",
 * or "lcd: probe what?" for a line it does not take.
 *
 * The timing scene reads the controller on its data line, turned round, and writes what it answers:
 * RDDID, RDDST, RDDPM and GSCAN as raw words,
 * and, for three of FRCTRL2's frame rates, the cycles over TIMING_FRAMES frames and the highest line the scan reached.
 *
 * Build and run: make BOARD=esp32c5 PORT=<port> lcd LCD_CONFIG="scene=stress mhz=80"
 * The pins are the ESP32-C5's, see user/board/esp32c5/devices.h; no other board builds it.
 * Once it has drawn it halts the machine with code 0, and the panel keeps the picture until the chip is reset.
 */

#include <stdint.h>

#include "bus.h"
#include "console.h"
#include "devices.h"
#include "app.h"
#include "spi2.h"
#include "st7789.h"
#include "rvuos.h"

/* The root task's own slots, past the boot's. */
enum {
    SLOT_NTFN = BOOT_CAP_COUNT, /* the notification the timer signals */
    SLOT_LINE,                  /* the timer line, carved */
    SLOT_TIMER,                 /* the Irq on it */
    SLOT_CONSOLE,               /* the Irq on the console's line, for the probe */
};

/* Region slots of the root task's process past its code and data. */
#define REGION_CONSOLE 2
#define REGION_IO_MUX  3
#define REGION_GPIO    4
#define REGION_SPI2    5
#define REGION_INPUT   6
#define REGION_RNG     7

#define BIT_TIMER   1u
#define BIT_CONSOLE 2u


static void puts(const char *s)
{
    while (*s != '\0') {
        console_put_polled(*s++);
    }
}

static void put_dec(uint64_t v)
{
    char digits[20];
    unsigned n = 0;
    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v != 0);
    while (n > 0) {
        console_put_polled(digits[--n]);
    }
}

static void put_hex(uint32_t v)
{
    puts("0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        console_put_polled("0123456789abcdef"[(v >> shift) & 0xfu]);
    }
}

static void expect(const char *what, uint32_t status)
{
    if (status != KERR_OK) {
        rv_puts(BOOT_CAP_PRINT, "lcd: ");
        rv_puts(BOOT_CAP_PRINT, what);
        rv_puts(BOOT_CAP_PRINT, ": FAILED\n");
        rv_halt(BOOT_CAP_HALT, 1);
    }
}

void lcd_bus_sleep(uint32_t us)
{
    uint32_t bits;
    expect("set the timer", rv_timer_set(SLOT_TIMER, BIT_TIMER, us));
    expect("wait for it", rv_wait(SLOT_NTFN, &bits));
}

/* The value of key= in the configuration of n bytes at conf, its first byte and its length in *len, or 0 if it has none. */
static const char *config(const char *conf, uint32_t n, const char *key, uint32_t *len)
{
    uint32_t k = 0;
    while (key[k] != '\0') {
        k++;
    }
    for (uint32_t i = 0; i + k < n && conf[i] != '\0'; i++) {
        uint32_t j = 0;
        while (j < k && conf[i + j] == key[j]) {
            j++;
        }
        if (j == k && conf[i + k] == '=' && (i == 0 || conf[i - 1] == ' ' || conf[i - 1] == '\n')) {
            const char *v = conf + i + k + 1;
            *len = 0;
            while (v + *len < conf + n && v[*len] > ' ') {
                (*len)++;
            }
            return v;
        }
    }
    return 0;
}

static uint32_t config_number(const char *conf, uint32_t n, const char *key, uint32_t dflt)
{
    uint32_t len;
    const char *v = config(conf, n, key, &len);
    if (v == 0 || len == 0) {
        return dflt;
    }
    uint32_t value = 0;
    for (uint32_t i = 0; i < len; i++) {
        if (v[i] < '0' || v[i] > '9') {
            return dflt;
        }
        value = value * 10 + (uint32_t)(v[i] - '0');
    }
    return value;
}

static uint64_t cycles(void)
{
    uint64_t now;
    uint32_t hz;
    expect("read the clock", rv_clock_read(BOOT_CAP_CLOCK, &now, &hz));
    return now;
}

/* The clock's rate, in cycles a second. */
static uint64_t clock_hz(void)
{
    uint64_t now;
    uint32_t hz;
    expect("read the clock", rv_clock_read(BOOT_CAP_CLOCK, &now, &hz));
    return hz;
}

/* A word from the chip's random number generator, for the noise scene. */
static uint32_t random_word(void)
{
    return *(volatile const uint32_t *)RNG_DATA_SYNC;
}

static void rng_print(const char *when)
{
    puts("lcd: RNG_CFG ");
    puts(when);
    puts(" ");
    put_hex(*(volatile const uint32_t *)RNG_CFG);
    puts("\n");
}

/* How many frames the timing scene times a frame rate over. */
#define TIMING_FRAMES 20u

/* The 32 bits the controller answers command c with, the first in the top bit. */
static uint32_t answer(uint8_t c)
{
    lcd_bus_command(c);
    uint32_t v = lcd_bus_read();
    lcd_bus_end();
    return v;
}

/*
 * The timing scene: what the controller answers its reads with, as raw words, and how fast it refreshes.
 * The commands of the controller's second table, PORCTRL's and FRCTRL2's among them, do not answer a read so:
 * they take its 32 clocks, with the data line let go and high, as parameters of 0xff, and the refresh changes with them.
 */
static void timing_run(void)
{
    static const struct {
        uint8_t command;
        const char *name;
    } reads[] = { { 0x04, "RDDID" }, { 0x09, "RDDST" }, { 0x0a, "RDDPM" }, { 0x45, "GSCAN" } };
    for (uint32_t i = 0; i < sizeof(reads) / sizeof(reads[0]); i++) {
        puts("lcd: ");
        puts(reads[i].name);
        puts(" ");
        put_hex(answer(reads[i].command));
        puts("\n");
    }
    /*
     * For three of FRCTRL2's frame rates, the one the controller starts with last:
     * the cycles over TIMING_FRAMES frames, from the scan's first wrap to its last, and the highest line it reached.
     */
    static const uint8_t rates[] = { 0x00, 0x1f, 0x0f };
    for (uint32_t r = 0; r < sizeof(rates); r++) {
        lcd_bus_command(0xc6);
        lcd_bus_data(&rates[r], 1);
        lcd_bus_end();
        uint32_t last = st7789_scanline(), top = 0, wraps = 0;
        uint64_t first = 0, now = 0;
        while (wraps <= TIMING_FRAMES) {
            uint32_t line = st7789_scanline();
            now = cycles();
            if (line < last) {
                if (wraps == 0) {
                    first = now;
                }
                wraps++;
            }
            if (line > top) {
                top = line;
            }
            last = line;
        }
        puts("lcd: FRCTRL2 ");
        put_hex(rates[r]);
        puts(": ");
        put_dec(now - first);
        puts(" cycles over ");
        put_dec(TIMING_FRAMES);
        puts(" frames, lines up to ");
        put_dec(top);
        puts("\n");
    }
}

/* The probe's state: the LCD, and whether a pixel is lit, and where. */
struct probe {
    const struct st7789 *lcd;
    int lit;
    uint32_t x, y;
};

static void probe_show(const struct probe *p, int on)
{
    st7789_fill(p->lcd, p->x, p->y, p->x, p->y, on ? RGB565(255, 255, 255) : RGB565(0, 0, 0));
}

/* A number at *s, moving *s past it; *ok cleared if there is none. */
static uint32_t probe_number(const char **s, int *ok)
{
    while (**s == ' ') {
        (*s)++;
    }
    uint32_t v = 0, digits = 0;
    while (**s >= '0' && **s <= '9' && digits < 4) {
        v = v * 10 + (uint32_t)(**s - '0');
        (*s)++;
        digits++;
    }
    if (digits == 0) {
        *ok = 0;
    }
    return v;
}

static void probe_obey(struct probe *p, const char *line)
{
    if (line[0] == 'e' && line[1] == 'n' && line[2] == 'd' && line[3] == '\0') {
        puts("lcd: probe end\n");
        console_flush();
        rv_halt(BOOT_CAP_HALT, 0);
    }
    if (line[0] == 'm' && line[1] == 'a' && line[2] == 'r' && line[3] == 'k' && line[4] == ' ') {
        int ok = 1;
        const char *s = line + 4;
        uint32_t x = probe_number(&s, &ok), y = probe_number(&s, &ok);
        if (!ok || *s != '\0' || x >= LCD_W || y >= LCD_H) {
            puts("lcd: probe what?\n");
            return;
        }
        st7789_fill(p->lcd, x, y, x, y, RGB565(255, 255, 255));
        puts("lcd: probe mark ");
        put_dec(x);
        puts(" ");
        put_dec(y);
        puts("\n");
        return;
    }
    if (p->lit) {
        probe_show(p, 0);
        p->lit = 0;
    }
    if (line[0] == 'o' && line[1] == 'f' && line[2] == 'f' && line[3] == '\0') {
        puts("lcd: probe off\n");
        return;
    }
    int ok = 1;
    const char *s = line;
    uint32_t x = probe_number(&s, &ok), y = probe_number(&s, &ok);
    if (!ok || *s != '\0' || x >= LCD_W || y >= LCD_H) {
        puts("lcd: probe what?\n");
        return;
    }
    p->x = x;
    p->y = y;
    p->lit = 1;
    probe_show(p, 1);
    puts("lcd: probe ");
    put_dec(x);
    puts(" ");
    put_dec(y);
    puts("\n");
}

/* The probe's loop, which never returns: the host's lines on the console. */
static void probe_run(const struct st7789 *lcd)
{
    expect("carve the console's line", rv_irq_carve(BOOT_CAP_IRQ_LINES, CONSOLE_IRQ, 1, SLOT_CONSOLE));
    expect("bind the console's line", rv_irq_bind(SLOT_CONSOLE, BOOT_CAP_POOL, SLOT_NTFN, SLOT_CONSOLE));
    console_listen();
    expect("arm the console", rv_irq_set(SLOT_CONSOLE, BIT_CONSOLE));
    puts("lcd: probing\n");
    console_flush();

    struct probe p = { .lcd = lcd };
    char line[16];
    uint32_t n = 0;
    for (;;) {
        uint32_t bits;
        expect("wait", rv_wait(SLOT_NTFN, &bits));
        if (bits & BIT_CONSOLE) {
            char c;
            console_listen();
            while (console_get(&c)) { /* bounded: a packet's bytes */
                if (c == '\n' || c == '\r') {
                    line[n] = '\0';
                    if (n != 0) {
                        probe_obey(&p, line);
                    }
                    n = 0;
                } else if (n + 1 < sizeof(line)) {
                    line[n++] = c;
                }
            }
            console_flush();
            expect("arm the console", rv_irq_set(SLOT_CONSOLE, BIT_CONSOLE));
        }
    }
}

/* The anim scene's report: each phase's rate, put whole and in hundredths, as a phase's cycles fit in 32 bits. */
static void anim_print(const struct anim_report *report)
{
    uint32_t hz = (uint32_t)clock_hz();
    for (uint32_t phase = 0; phase < ANIM_PHASES; phase++) {
        uint32_t per = (uint32_t)report->cycles[phase] / ANIM_PICTURES;
        puts("lcd: phase ");
        put_dec(phase);
        puts(phase % 2 ? ", in sync: " : ", not in sync: ");
        put_dec(ANIM_PICTURES);
        puts(" pictures in ");
        put_dec(report->cycles[phase]);
        puts(" cycles, ");
        put_dec(hz / per);
        puts(".");
        put_dec((hz % per) * 100u / per / 10u);
        put_dec((hz % per) * 100u / per % 10u);
        puts(" pictures a second\n");
        puts("lcd:   from start to start ");
        put_dec(report->shortest_gap[phase]);
        puts(" to ");
        put_dec(report->longest_gap[phase]);
        puts(" cycles, a picture written in ");
        put_dec(report->longest_write[phase]);
        puts(" at most, ");
        put_dec(report->least_slack[phase]);
        puts(" at least from one's end to the next one's start\n");
    }
}

int main(void);
int main(void)
{
    uint32_t base, size;
    expect("console info", rv_frame_info(BOOT_CAP_UART, &base, &size));
    expect("map the console", rv_process_install(BOOT_CAP_PROCESS, REGION_CONSOLE, BOOT_CAP_UART, RIGHT_R | RIGHT_W));
    console_init(base);
    expect("map the IO MUX", rv_process_install(BOOT_CAP_PROCESS, REGION_IO_MUX, BOOT_CAP_IO_MUX, RIGHT_R | RIGHT_W));
    expect("map the pins", rv_process_install(BOOT_CAP_PROCESS, REGION_GPIO, BOOT_CAP_GPIO, RIGHT_R | RIGHT_W));
    expect("map SPI2", rv_process_install(BOOT_CAP_PROCESS, REGION_SPI2, BOOT_CAP_SPI2, RIGHT_R | RIGHT_W));
    expect("map the RNG", rv_process_install(BOOT_CAP_PROCESS, REGION_RNG, BOOT_CAP_RNG, RIGHT_R));

    expect("allocate the timer's notification", rv_pool_alloc(BOOT_CAP_POOL, CAP_NOTIFICATION, SLOT_NTFN, 0));
    expect("carve a timer line", rv_irq_carve(BOOT_CAP_TIMER_LINES, 0, 1, SLOT_LINE));
    expect("bind the timer line", rv_irq_bind(SLOT_LINE, BOOT_CAP_POOL, SLOT_NTFN, SLOT_TIMER));

    expect("input", rv_frame_info(BOOT_CAP_INPUT, &base, &size));
    expect("map the input", rv_process_install(BOOT_CAP_PROCESS, REGION_INPUT, BOOT_CAP_INPUT, RIGHT_R));
    const char *conf = (const char *)(uintptr_t)base;
    uint32_t len;
    const char *name = config(conf, size, "scene", &len);
    int scene = name != 0 ? app_scene(name, len) : APP_PATTERN;
    uint32_t mhz = config_number(conf, size, "mhz", 40) == 80 ? 80 : 40;
    uint32_t drive = config_number(conf, size, "drive", 2) & 3u;
    struct app_options options = APP_OPTIONS_DEFAULT;
    options.madctl = config_number(conf, size, "madctl", ORIENT_ALL);
    options.backlight = config_number(conf, size, "backlight", LCD_BACKLIGHT_FULL);
    struct anim_report report;
    options.clock = cycles;
    options.report = &report;
    options.random = random_word;
    options.pictures = config_number(conf, size, "pictures", NOISE_PICTURES);
    if (options.backlight > LCD_BACKLIGHT_FULL) {
        options.backlight = LCD_BACKLIGHT_FULL;
    }
    if (options.madctl > 0xffu) {
        options.madctl = ORIENT_ALL;
    }
    if (scene < 0) {
        puts("lcd: no such scene, so the pattern\n");
        scene = APP_PATTERN;
    }
    puts("lcd: scene ");
    put_dec((uint32_t)scene);
    puts(", bus at ");
    put_dec(mhz);
    puts(" MHz, drive ");
    put_dec(drive);
    puts(", backlight ");
    put_dec(options.backlight);
    if (scene == APP_ORIENT && options.madctl == ORIENT_ALL) {
        puts(", each MADCTL in turn");
    } else if (scene == APP_ORIENT) {
        puts(", MADCTL ");
        put_dec(options.madctl);
    }
    puts("\n");

    if (scene == APP_NOISE) {
        rng_print("before");
    }
    lcd_bus_init();
    spi2_configure(mhz, drive);
    uint64_t t0 = cycles();
    struct st7789 lcd;
    app_run(&lcd, scene, &options);
    uint64_t t1 = cycles();

    puts("lcd: drawn in ");
    put_dec(t1 - t0);
    puts(" cycles\n");
    if (scene == APP_PROBE) {
        probe_run(&lcd);
    } else if (scene == APP_TIMING) {
        timing_run();
    } else if (scene == APP_ANIM) {
        anim_print(&report);
    } else if (scene == APP_NOISE) {
        rng_print("after");
    }
    rv_halt(BOOT_CAP_HALT, 0);
    return 0;
}
