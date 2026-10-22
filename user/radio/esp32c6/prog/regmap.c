/*
 * The register map: every word of the modem's frame, 0x600A0000 to 0x600B0000, read and told, in four passes:
 * cold, as the program finds the modem, which a reset leaves as the image before left it;
 * up, once the MAC and the PHY are up as the station's are, mac_up, receiving on channel=, 1 by default;
 * again, the same a moment later, so that what moves by itself shows;
 * and tuned, on the channel five above, so that what the channel sets shows.
 * Each word is read twice in a row, and a line's mask says which of its words moved between the two reads,
 * and each 4 KiB page's reads are timed, the telling left out, since a block may answer only after the bus has waited.
 * The analog I2C's blocks are read in the last three passes alone, as the PHY's bring-up powers their bus.
 * The program only reads, but a read may have an effect of its own, as a FIFO's or a status cleared on reading has,
 * so what it tells of the passes after one such is what the reads left; it sends nothing.
 *
 * Run it with make BOARD=esp32c6 radio PROG=regmap RADIO_CONFIG=file, see build.mk,
 * and read the passes with tools/regmap.py.
 */

#include <stdarg.h>
#include <stdint.h>

#include "devices.h"
#include "drv.h"
#include "mac.h"
#include "osi.h"
#include "rvuos.h"
#include "wifi/conf.h"

DRV_PROGRAM("the register map", 0);

#define FRAME_SIZE 0x10000u
#define PAGE_SIZE  0x1000u
#define LINE_WORDS 8u
/* Lines a burst, about 1.5 KiB of the kernel's log of 4, and the pause after each, as dump.c's hex. */
#define BURST    16u
#define PAUSE_MS 10u
#define I2C_REGS 32u /* the registers read of each block, as many as any is known to have and more */

/* The analog I2C's blocks and their hosts, as libphy's phy_i2c_check walks them; hw/phy.md names them. */
static const uint8_t i2c_blocks[][2] = {
    { 0x61, 0 }, { 0x62, 1 }, { 0x63, 1 }, { 0x66, 0 }, { 0x67, 1 }, { 0x69, 0 }, { 0x6a, 1 }, { 0x6b, 1 }, { 0x6d, 0 },
};

static int up;
static struct drv *self_page;
static uint32_t told; /* lines told since the last pause */

void drv_radio_off(void)
{
    if (up) {
        up = 0;
        mac_stop();
    }
}

/* A line told, and a pause every BURST of them, for the root task to carry them out and to find the program alive. */
static void tell(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    drv_vsay(0, fmt, args);
    va_end(args);
    if (++told % BURST == 0) {
        child_answer(&self_page->c);
        osi_delay_ms(PAUSE_MS);
    }
}

/*
 * One pass over the frame. A run of lines equal to the one before, their masks clear, is told as one line, "same n",
 * so that the log holds every line in order and tools/regmap.py can tell a line lost from a run left out.
 */
static void pass(const char *name)
{
    uint32_t last[LINE_WORDS] = { 0 }, same = 0, lines = 0;
    uint64_t reading = 0;
    for (uint32_t at = 0; at < FRAME_SIZE; at += LINE_WORDS * 4u) { /* bounded by FRAME_SIZE */
        uint32_t w[LINE_WORDS], moved = 0, equal = at != 0;
        child_answer(&self_page->c); /* a line may take long, see the page's time */
        uint64_t start = osi_now_us();
        for (uint32_t k = 0; k < LINE_WORDS; k++) {
            volatile uint32_t *a = (volatile uint32_t *)(uintptr_t)(MODEM_BASE + at + 4u * k);
            uint32_t first = *a;
            w[k] = *a;
            moved |= (uint32_t)(first != w[k]) << k;
            equal &= w[k] == last[k];
        }
        reading += osi_now_us() - start;
        if (!equal || moved != 0) {
            if (same) {
                tell("regmap: %s %x same %u\n", name, MODEM_BASE + at, same);
                same = 0;
            }
            tell("regmap: %s %x %x %x %x %x %x %x %x %x %x\n", name, MODEM_BASE + at, moved, w[0], w[1], w[2], w[3],
                 w[4], w[5], w[6], w[7]);
            lines++;
            for (uint32_t k = 0; k < LINE_WORDS; k++) {
                last[k] = w[k];
            }
        } else {
            same++;
        }
        if ((at + LINE_WORDS * 4u) % PAGE_SIZE == 0) {
            tell("regmap: %s page %x us %u\n", name, MODEM_BASE + at + LINE_WORDS * 4u - PAGE_SIZE, (uint32_t)reading);
            reading = 0;
        }
    }
    if (same) {
        tell("regmap: %s %x same %u\n", name, MODEM_BASE + FRAME_SIZE, same);
    }
    tell("regmap: %s done %u\n", name, lines);
}

static void pass_i2c(const char *name)
{
    for (uint32_t b = 0; b < sizeof(i2c_blocks) / sizeof(i2c_blocks[0]); b++) {
        uint8_t r[I2C_REGS];
        for (uint32_t k = 0; k < I2C_REGS; k++) {
            r[k] = drv_phy_i2c_read(i2c_blocks[b][0], i2c_blocks[b][1], (uint8_t)k);
        }
        tell("regmap: %s i2c %x %u %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x\n", name, i2c_blocks[b][0],
             i2c_blocks[b][1], r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[13],
             r[14], r[15]);
        tell("regmap: %s i2c %x %u+ %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x\n", name, i2c_blocks[b][0],
             i2c_blocks[b][1], r[16], r[17], r[18], r[19], r[20], r[21], r[22], r[23], r[24], r[25], r[26], r[27],
             r[28], r[29], r[30], r[31]);
    }
}

void drv_run(struct drv *d, struct self *own)
{
    (void)own;
    self_page = d;
    uint32_t channel = config_number(d->conf, d->conf_len, "channel", 1);
    drv_must("a channel of 1 to 8", channel >= 1 && channel <= 8 ? ESP_OK : ESP_FAIL);
    pass("cold");
    mac_up(d->mac);
    up = 1;
    drv_must_mac("the channel", mac_channel(channel));
    d->channel = channel;
    child_report(&d->c, DRV_LISTENING);
    pass("up");
    pass_i2c("up");
    pass("again");
    pass_i2c("again");
    drv_must_mac("the channel five above", mac_channel(channel + 5u));
    pass("tuned");
    pass_i2c("tuned");
    drv_say("regmap: the map is told\n");
    for (;;) {
        uint32_t bits;
        child_answer(&d->c);
        if (d->leave) {
            break;
        }
        rv_wait(CHILD_INBOX, &bits);
    }
    drv_radio_off();
    drv_say("regmap: the radio is off\n");
    child_stop(&d->c, DRV_LEFT);
}
