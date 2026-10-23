/*
 * The BBPLL moved: which of the radio's clocks come from it, see hw/dump.md, "The clocks' source".
 * The 40 MHz crystal drives the BBPLL at 480 MHz, 40 MHz times its feedback divider OC_DIV_7_0 plus 4, 8 + 4,
 * and the processor, the modem's clocks and the dump engine's are fixed divisions of it.
 * The BBPLL's registers, block 0x66 of the analog I2C, are told once at the start.
 * Each second, with nothing told from its start to its end, as the console's USB is clocked from the PLL too:
 * a capture as the PLL stands, the control; then one of sets=, in turn, a register of the block written,
 * hexadecimal reg:value, the byte, or reg^mask, the bits of the mask turned over from what the register held,
 * register 3, the feedback divider, only written, 4 to a, 320 to 560 MHz, what was seen to lock,
 * commas between, 3:7 by default, 440 MHz;
 * the PLL calibrated again as ESP-IDF's rtc_clk_bbpll_configure calibrates it, and its register 8 read,
 * the calibration's capacitor, its underflow and overflow, its end and the lock;
 * a capture of capwords= words, 16383 by default, the processor still running from the PLL, as a program reaches no PCR;
 * and the byte written back, calibrated again.
 * Of each capture its first HEX_WORDS words are told, and of the moved one's its last too:
 * a line from outside of known frequency in them, the nRF52840's carrier, carrier= of user/thread/root.c,
 * tells the dump engine's clock against the crystal's, whether the local oscillator moved, and whether the PLL had settled.
 * tonemoved=1 also loops the transmit tone generator's tone back while a set holds, step 0x80,
 * which lets it out at the local oscillator's frequency, so only once a run without it has shown that unmoved.
 * The MAC and the PHY come up as the station's do, mac_up, on channel=, 1 by default, 2412 MHz.
 *
 * Run it with make BOARD=esp32c6 radio PROG=bbpll RADIO_CONFIG=file, see build.mk.
 */

#include <stdint.h>

#include "devices.h"
#include "drv.h"
#include "dump.h"
#include "mac.h"
#include "osi.h"
#include "rvuos.h"
#include "wifi/conf.h"

DRV_PROGRAM("the BBPLL moved", DRV_NEED_DUMP);

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))

#define DUMP_CTRL       (MODEM_BASE + 0x9004u)
#define CTRL_DONE       (1u << 18)
#define CTRL_TRIGGER    (1u << 19)
#define CTRL_ENABLE     (1u << 31)
#define ANA_CONF0       (I2C_ANA_MST_BASE + 0x18u) /* I2C_MST_ANA_CONF0 */
#define BBPLL_STOP_HIGH (1u << 2)
#define BBPLL_STOP_LOW  (1u << 3)
#define BBPLL_CAL_DONE  (1u << 24)
#define I2C_BBPLL       0x66u
#define I2C_BBPLL_HOST  0u
#define BBPLL_REGS      11u /* 0 to 10, as ESP-IDF's regi2c_bbpll.h names them */
#define BBPLL_STATUS    8u  /* OR_LOCK 7, OR_CAL_END 6, OR_CAL_OVF 5, OR_CAL_UDF 4, OR_CAL_CAP 3 to 0 */
#define BBPLL_DIV_7_0   3u  /* OC_DIV_7_0: the PLL at 40 MHz times it and 4 */
#define DIV_LEAST       4u  /* 320 MHz */
#define DIV_MOST        10u /* 560 MHz */
#define POLLS_MAX       1000000u
#define HEX_WORDS       1024u
#define SETS_MAX        16u
#define TONE_STEP       0x80u /* +5.00 MHz at 480, hw/phy.md "The loopback" */
#define TONE_ATTEN      0x30u
#define LOOP_GAIN       0x20u

/* The ROM's table of its PHY functions, as phy.c reaches it; slot 22 is rom_i2c_writeReg. */
typedef void (*romfunc)(void);
romfunc *phy_get_romfuncs(void);
#define ROMFUNC_I2C_WRITE_REG 22

struct set {
    uint8_t reg, value, flip; /* flip: value is a mask of bits turned over */
};

static volatile uint32_t *const bank = (volatile uint32_t *)(uintptr_t)DUMP_BANK_BASE;
static uint32_t kept[2u * HEX_WORDS]; /* the control's first words and the moved one's; its last stay in the bank */
static uint32_t captures;
static int up, looped, toned, moved;
static uint8_t moved_reg, moved_was;

static void i2c_write(uint8_t reg, uint8_t data)
{
    ((void (*)(uint8_t, uint8_t, uint8_t, uint8_t))phy_get_romfuncs()[ROMFUNC_I2C_WRITE_REG])(I2C_BBPLL, I2C_BBPLL_HOST,
                                                                                               reg, data);
}

static uint8_t i2c_read(uint8_t reg)
{
    return drv_phy_i2c_read(I2C_BBPLL, I2C_BBPLL_HOST, reg);
}

/*
 * A byte of the BBPLL's written, as ESP-IDF's rtc_clk_bbpll_configure writes its divider, the calibration held between:
 * whether the calibration said it was done within POLLS_MAX.
 */
static int bbpll_write(uint8_t reg, uint8_t value)
{
    uint32_t polls = 0;
    REG(ANA_CONF0) = (REG(ANA_CONF0) & ~BBPLL_STOP_HIGH) | BBPLL_STOP_LOW;
    i2c_write(reg, value);
    while ((REG(ANA_CONF0) & BBPLL_CAL_DONE) == 0 && polls < POLLS_MAX) { /* bounded by POLLS_MAX */
        polls++;
    }
    for (volatile uint32_t k = 0; k < 2000u; k++) { /* some 10 us, ESP-IDF's wait for the calibration's true stop */
    }
    REG(ANA_CONF0) = (REG(ANA_CONF0) & ~BBPLL_STOP_LOW) | BBPLL_STOP_HIGH;
    return polls < POLLS_MAX;
}

void drv_radio_off(void)
{
    if (moved) {
        moved = 0;
        (void)bbpll_write(moved_reg, moved_was);
    }
    if (toned) {
        toned = 0;
        drv_phy_tone(0, 0, 0);
    }
    if (looped) {
        looped = 0;
        drv_phy_loopback(0, 0);
    }
    if (up) {
        up = 0;
        mac_stop();
    }
}

/* A capture of n words from source 15 at the engine's whole clock, at once: its microseconds of the kernel's clock. */
static uint32_t capture(uint32_t n)
{
    uint32_t polls = 0, usage = dump_bank_give();
    REG(DUMP_CTRL) = n | CTRL_DONE;
    REG(DUMP_CTRL) = n | CTRL_ENABLE;
    uint64_t start = osi_now_us();
    REG(DUMP_CTRL) = n | CTRL_ENABLE | CTRL_TRIGGER;
    REG(DUMP_CTRL) = n | CTRL_ENABLE;
    while ((REG(DUMP_CTRL) & CTRL_DONE) == 0 && polls < POLLS_MAX) { /* bounded by POLLS_MAX */
        polls++;
    }
    uint32_t us = (uint32_t)(osi_now_us() - start);
    REG(DUMP_CTRL) = 0;
    dump_bank_take(usage);
    return us;
}

static void keep(uint32_t *to, uint32_t from)
{
    for (uint32_t k = 0; k < HEX_WORDS; k++) {
        to[k] = bank[from + k];
    }
}

/* n words as dump.c tells a capture, for user/radio/esp32c6/tools/dump-spectrum.py and the like. */
static void tell(const char *what, const volatile uint32_t *w, uint32_t n)
{
    drv_say("dump: capture %u source 15 lane 0 rate 0: %s\n", captures, what);
    for (uint32_t k = 0, lines = 0; k + 8u <= n; k += 8u) { /* bounded by n */
        if (lines++ % 16u == 0) {
            osi_delay_ms(10);
        }
        drv_say("dumphex: %u %u %x %x %x %x %x %x %x %x\n", captures, k, w[k], w[k + 1u], w[k + 2u], w[k + 3u],
                w[k + 4u], w[k + 5u], w[k + 6u], w[k + 7u]);
    }
    captures++;
}

static uint32_t hex_digit(char c)
{
    return c >= '0' && c <= '9' ? (uint32_t)(c - '0') : c >= 'a' && c <= 'f' ? (uint32_t)(c - 'a' + 10) : 16u;
}

/* sets=, as above, up to SETS_MAX; how many, 3:7 if the key is absent. */
static uint32_t sets_of(struct drv *d, struct set *s)
{
    char list[128];
    uint32_t n = 0;
    list[0] = '\0';
    config_value(d->conf, d->conf_len, "sets", list, sizeof(list));
    if (list[0] == '\0') {
        s[0] = (struct set){ 3, 7, 0 };
        return 1;
    }
    const char *c = list;
    for (; *c != '\0' && n < SETS_MAX;) {
        uint32_t reg = 0, value = 0, digits = 0;
        for (; hex_digit(*c) < 16u && digits < 2u; c++, digits++) {
            reg = reg << 4 | hex_digit(*c);
        }
        drv_must("sets= of reg:value or reg^mask", digits != 0 && (*c == ':' || *c == '^') && reg < BBPLL_REGS
                                                       ? ESP_OK : ESP_FAIL);
        uint8_t flip = *c++ == '^';
        for (digits = 0; hex_digit(*c) < 16u && digits < 2u; c++, digits++) {
            value = value << 4 | hex_digit(*c);
        }
        drv_must("a set's value of one or two hex digits", digits != 0 ? ESP_OK : ESP_FAIL);
        drv_must("no set of the status register 8", reg != BBPLL_STATUS ? ESP_OK : ESP_FAIL);
        /* The processor runs from the PLL: its feedback divider only within what was seen to lock, 320 to 560 MHz. */
        drv_must("a feedback divider, register 3, of DIV_LEAST to DIV_MOST",
                 reg != BBPLL_DIV_7_0 || flip || (value >= DIV_LEAST && value <= DIV_MOST) ? ESP_OK : ESP_FAIL);
        drv_must("no flip of register 3", reg != BBPLL_DIV_7_0 || !flip ? ESP_OK : ESP_FAIL);
        s[n++] = (struct set){ (uint8_t)reg, (uint8_t)value, flip };
        if (*c != ',') {
            drv_must("sets= separated by commas alone", *c == '\0' ? ESP_OK : ESP_FAIL);
            break;
        }
        c++;
    }
    drv_must("sets= of at most SETS_MAX", *c == '\0' ? ESP_OK : ESP_FAIL);
    return n;
}

void drv_run(struct drv *d, struct self *own)
{
    (void)own;
    uint32_t channel = config_number(d->conf, d->conf_len, "channel", 1);
    uint32_t capwords = config_number(d->conf, d->conf_len, "capwords", 16383);
    uint32_t tonemoved = config_number(d->conf, d->conf_len, "tonemoved", 0);
    struct set sets[SETS_MAX];
    uint32_t nsets = sets_of(d, sets);
    drv_must("a channel of 1 to 13", channel >= 1 && channel <= 13 ? ESP_OK : ESP_FAIL);
    drv_must("capwords= of 2 HEX_WORDS to 16383", capwords >= 2u * HEX_WORDS && capwords <= 16383u ? ESP_OK : ESP_FAIL);
    mac_up(d->mac);
    up = 1;
    dump_known();
    drv_must_mac("the channel", mac_channel(channel));
    d->channel = channel;
    child_report(&d->c, DRV_LISTENING);
    uint8_t regs[BBPLL_REGS];
    for (uint32_t r = 0; r < BBPLL_REGS; r++) {
        regs[r] = i2c_read((uint8_t)r);
    }
    drv_say("bbpll: channel %u, %u sets; registers 0 to 10: %x %x %x %x %x %x %x %x %x %x %x\n", channel, nsets, regs[0],
            regs[1], regs[2], regs[3], regs[4], regs[5], regs[6], regs[7], regs[8], regs[9], regs[10]);

    uint64_t told = osi_now_us();
    for (uint32_t second = 0;;) {
        uint32_t bits;
        child_answer(&d->c);
        if (d->leave) {
            break;
        }
        uint64_t now = osi_now_us();
        if (now - told >= 1000000u) {
            told = now;
            const struct set *s = &sets[second % nsets];
            /* Nothing is told from here to the PLL's return: the console's USB has the PLL's clock. */
            uint32_t control_us = capture(capwords);
            keep(kept, 0);
            uint8_t before = i2c_read(BBPLL_STATUS);
            moved_reg = s->reg;
            moved_was = i2c_read(s->reg);
            uint8_t value = s->flip ? (uint8_t)(moved_was ^ s->value) : s->value;
            moved = 1;
            uint32_t calibrated = (uint32_t)bbpll_write(s->reg, value);
            uint8_t status = i2c_read(BBPLL_STATUS), readback = i2c_read(s->reg);
            uint32_t moved_us = capture(capwords);
            keep(&kept[HEX_WORDS], 0);
            uint32_t tone_us = 0;
            if (tonemoved) {
                drv_phy_loopback(1, LOOP_GAIN);
                looped = 1;
                drv_phy_tone(1, TONE_STEP, TONE_ATTEN);
                toned = 1;
                tone_us = capture(HEX_WORDS);
                drv_phy_tone(0, 0, 0);
                toned = 0;
                drv_phy_loopback(0, 0);
                looped = 0;
            }
            moved = 0;
            uint32_t back = (uint32_t)bbpll_write(moved_reg, moved_was);
            uint8_t after = i2c_read(BBPLL_STATUS);
            drv_say("bbpll: second %u: register %u %x for %x, read back %x, calibrated %u, status %x then %x then %x; "
                    "%u words in %u us before and %u us so; the tone's %u us; back %u\n",
                    second, s->reg, value, moved_was, readback, calibrated, before, status, after, capwords,
                    control_us, moved_us, tone_us, back);
            tell("before, the control", kept, HEX_WORDS);
            tell("so, its start", &kept[HEX_WORDS], HEX_WORDS);
            tell("so, its end", &bank[capwords - HEX_WORDS], HEX_WORDS);
            if (tonemoved) {
                tell("so, looped back, the tone", bank, HEX_WORDS);
            }
            second++;
        }
        rv_wait(CHILD_INBOX, &bits);
    }
    drv_radio_off();
    drv_say("bbpll: the radio is off\n");
    child_stop(&d->c, DRV_LEFT);
}
