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
 * Build and run: make BOARD=esp32c5 build/esp32c5/kernel-lcd.bin
 *                tools/esp32c6-run.py --chip esp32c5 --port <port> --timeout 5 build/esp32c5/kernel-lcd.bin
 * The pins are the ESP32-C5's, see user/board/esp32c5/devices.h; no other board builds it.
 * Once it has drawn it halts the machine with code 0, and the panel keeps the picture until the chip is reset.
 */

#include <stdint.h>

#include "bus.h"
#include "console.h"
#include "devices.h"
#include "app.h"
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

    lcd_bus_init();
    uint32_t t0 = cycles();
    app_run();
    uint32_t t1 = cycles();

    puts("lcd: drawn in ");
    put_dec(t1 - t0);
    puts(" cycles\n");
    rv_halt(BOOT_CAP_HALT, 0);
    return 0;
}
