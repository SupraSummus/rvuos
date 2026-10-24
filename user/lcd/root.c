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
 * scene= names app.c's scene, pattern unless given, mhz= the bus's clock, 40 or 80, and drive= its pins' drive, 0 to 3;
 * see spi2.h. What it does not find it leaves at the default.
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
#include "rvuos.h"

/* The root task's own slots, past the boot's. */
enum {
    SLOT_NTFN = BOOT_CAP_COUNT, /* the notification the timer signals */
    SLOT_LINE,                  /* the timer line, carved */
    SLOT_TIMER,                 /* the Irq on it */
};

/* Region slots of the root task's process past its code and data. */
#define REGION_CONSOLE 2
#define REGION_IO_MUX  3
#define REGION_GPIO    4
#define REGION_SPI2    5
#define REGION_INPUT   6

#define BIT_TIMER 1u

static void puts(const char *s)
{
    while (*s != '\0') {
        console_put_polled(*s++);
    }
}

static void put_dec(uint32_t v)
{
    char digits[10];
    unsigned n = 0;
    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v != 0);
    while (n > 0) {
        console_put_polled(digits[--n]);
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

static uint32_t cycles(void)
{
    uint64_t now;
    uint32_t hz;
    expect("read the clock", rv_clock_read(BOOT_CAP_CLOCK, &now, &hz));
    return (uint32_t)now;
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
    puts("\n");

    lcd_bus_init();
    spi2_configure(mhz, drive);
    uint32_t t0 = cycles();
    app_run(scene);
    uint32_t t1 = cycles();

    puts("lcd: drawn in ");
    put_dec(t1 - t0);
    puts(" cycles\n");
    rv_halt(BOOT_CAP_HALT, 0);
    return 0;
}
