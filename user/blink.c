/*
 * Blink: a root task that blinks the board's LED to a rhythm, "C W, C W K, C W K S, Le-gia!", for ever.
 *
 * The LED is a pin of the GPIO frame the boot grants, which the root task maps as any other region.
 * Each syllable is a flash some steps long, then a gap some steps long,
 * and the steps are a timer line kept on a period, so the tempo does not drift with what each turn costs.
 * It writes each syllable to the console as it flashes, so the runner shows it too.
 * The pins it names are nRF52840's, see user/board/nrf52840/devices.h; no other board builds it.
 *
 * Build and run: make BOARD=nrf52840 build/nrf52840/kernel-blink.elf
 *                tools/nrf52840-run.py build/nrf52840/kernel-blink.elf
 * The program runs on once the runner is stopped, until the chip is reset.
 */

#include <stdint.h>

#include "console.h"
#include "devices.h"
#include "rvuos.h"

/* The root task's own slots, past the boot's. */
enum {
    SLOT_NTFN = BOOT_CAP_COUNT, /* the notification the timer signals */
    SLOT_LINE,                  /* the timer line, carved */
    SLOT_TIMER,                 /* the Irq on it */
};

/* Region slots of the root task's process past its code and data. */
#define REGION_CONSOLE 2
#define REGION_GPIO    3

#define BIT_TIMER 1u
#define STEP_US 150000u

#define REG(addr) (*(volatile uint32_t *)(addr))

/* A syllable: what the console says, and the steps the LED is lit and then dark. */
struct syllable {
    const char *say;
    uint8_t lit;
    uint8_t dark;
};

static const struct syllable rhythm[] = {
    { "C ", 1, 1 }, { "W\n", 1, 3 },
    { "C ", 1, 1 }, { "W ", 1, 1 }, { "K\n", 1, 3 },
    { "C ", 1, 1 }, { "W ", 1, 1 }, { "K ", 1, 1 }, { "S\n", 1, 3 },
    { "Le-", 2, 1 }, { "gia!\n\n", 5, 6 },
};

static void puts(const char *s)
{
    while (*s != '\0') {
        console_put_polled(*s++);
    }
}

static void expect(const char *what, uint32_t status)
{
    if (status != KERR_OK) {
        rv_puts(BOOT_CAP_DEBUG, "blink: ");
        rv_puts(BOOT_CAP_DEBUG, what);
        rv_puts(BOOT_CAP_DEBUG, ": FAILED\n");
        rv_halt(BOOT_CAP_DEBUG, 1);
    }
}

/* steps of the beat, each counted from the deadline before. */
static void steps(uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        uint32_t skipped, bits;
        expect("keep the beat", rv_timer_period(SLOT_TIMER, BIT_TIMER, STEP_US, &skipped));
        expect("wait for it", rv_wait(SLOT_NTFN, &bits));
    }
}

int main(void);
int main(void)
{
    uint32_t base, size;
    expect("console info", rv_frame_info(BOOT_CAP_UART, &base, &size));
    expect("map the console", rv_process_install(BOOT_CAP_PROCESS, REGION_CONSOLE, BOOT_CAP_UART, RIGHT_R | RIGHT_W));
    console_init(base);
    expect("map the pins", rv_process_install(BOOT_CAP_PROCESS, REGION_GPIO, BOOT_CAP_GPIO, RIGHT_R | RIGHT_W));

    expect("allocate the timer's notification", rv_pool_alloc(BOOT_CAP_POOL, CAP_NOTIFICATION, SLOT_NTFN, 0));
    expect("carve a timer line", rv_irq_carve(BOOT_CAP_TIMER_LINES, 0, 1, SLOT_LINE));
    expect("bind the timer line", rv_irq_bind(SLOT_LINE, BOOT_CAP_POOL, SLOT_NTFN, SLOT_TIMER));

    /* Dark before it drives the pin, which is lit while low. */
    REG(LED_PORT_BASE + GPIO_OUTSET) = 1u << LED_PIN;
    REG(LED_PORT_BASE + GPIO_DIRSET) = 1u << LED_PIN;

    for (;;) {
        for (uint32_t i = 0; i < sizeof(rhythm) / sizeof(rhythm[0]); i++) {
            REG(LED_PORT_BASE + GPIO_OUTCLR) = 1u << LED_PIN;
            puts(rhythm[i].say);
            steps(rhythm[i].lit);
            REG(LED_PORT_BASE + GPIO_OUTSET) = 1u << LED_PIN;
            steps(rhythm[i].dark);
        }
    }
}
