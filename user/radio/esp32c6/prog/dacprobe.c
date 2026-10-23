/*
 * The probe of the dump engine's block for a player: an engine that would send what the bank holds,
 * as Espressif's RF test library's dactrig drives one on the ESP32-C3, see hw/dump.md, "A player".
 * The MAC and the PHY come up as the station's do, mac_up, the receiver on channel=, 6 by default,
 * and the transmitter stays powered down throughout: no tone, no loopback, no frame, so nothing a player started reaches the air.
 * The block's sixteen words, 0x600A9000 to 0x600A903C, are told as they are;
 * then, with write=1, each word of words=, a list of offsets in hexadecimal, is written in turn with value=,
 * the bank handed to the engines, their clock on, and holding a pattern; the block's words are read for PROBE_US,
 * every change from before the write told, the block told once more, and the word put back.
 * By default value= is what dactrig writes on the C3, count= samples, up to its 14 bits, and its enable bit, 31.
 *
 * Run it with make BOARD=esp32c6 radio PROG=dacprobe RADIO_CONFIG=file, see build.mk.
 */

#include <stdint.h>

#include "devices.h"
#include "drv.h"
#include "dump.h"
#include "mac.h"
#include "osi.h"
#include "rvuos.h"
#include "wifi/conf.h"

DRV_PROGRAM("the player's probe", DRV_NEED_DUMP);

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))

#define BLOCK       (MODEM_BASE + 0x9000u)
#define BLOCK_WORDS 16u
#define PROBE_US    2000u
#define CHANGES_MAX 12u /* changes told a word, the rest counted */
#define WORDS_MAX   8u
#define COUNT_MAX   0x3fffu /* the C3's count field, bits 0 to 13 */
#define ENABLE      (1u << 31)

static volatile uint32_t *const bank = (volatile uint32_t *)(uintptr_t)DUMP_BANK_BASE;
static int up;

void drv_radio_off(void)
{
    if (up) {
        up = 0;
        mac_stop();
    }
}

/* The block's words in one line, said at once, so that no other line of the log comes between them. */
static void tell_block(const char *when)
{
    uint32_t w[BLOCK_WORDS];
    for (uint32_t k = 0; k < BLOCK_WORDS; k++) {
        w[k] = REG(BLOCK + 4u * k);
    }
    drv_say("dacprobe: %s: %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x\n", when, w[0], w[1], w[2], w[3], w[4], w[5],
            w[6], w[7], w[8], w[9], w[10], w[11], w[12], w[13], w[14], w[15]);
}

/*
 * The pattern: a tone a sixteenth of the sample rate from the carrier, an amplitude of 64, from a table of one period,
 * its cosine in bits 10 to 19 and its sine in 0 to 9, as the capture's words hold I and Q;
 * the player takes I from bits 0 to 9, hw/dump.md "A player", so played it lies below the carrier.
 */
static const int16_t cosine16[16] = { 64, 59, 45, 24, 0, -24, -45, -59, -64, -59, -45, -24, 0, 24, 45, 59 };

static void fill(uint32_t count)
{
    for (uint32_t k = 0; k < count; k++) {
        uint32_t i = (uint32_t)cosine16[k % 16u] & 0x3ffu, q = (uint32_t)cosine16[(k + 12u) % 16u] & 0x3ffu;
        bank[k] = i << 10 | q;
    }
}

static void probe(uint32_t offset, uint32_t value, uint32_t count)
{
    uint32_t was[BLOCK_WORDS], seen[BLOCK_WORDS], changes = 0, told = 0;
    fill(count);
    uint32_t old = REG(BLOCK + offset);
    uint32_t usage = dump_bank_give();
    for (uint32_t k = 0; k < BLOCK_WORDS; k++) {
        seen[k] = REG(BLOCK + 4u * k);
    }
    REG(BLOCK + offset) = value;
    uint64_t start = osi_now_us(), now = start;
    while (now - start < PROBE_US) { /* bounded by PROBE_US */
        for (uint32_t k = 0; k < BLOCK_WORDS; k++) {
            uint32_t v = REG(BLOCK + 4u * k);
            if (v != seen[k]) {
                changes++;
                if (told++ < CHANGES_MAX) {
                    drv_say("dacprobe: +%u us: word %x %x to %x\n", (uint32_t)(now - start), 4u * k, seen[k], v);
                }
                seen[k] = v;
            }
        }
        now = osi_now_us();
    }
    for (uint32_t k = 0; k < BLOCK_WORDS; k++) {
        was[k] = REG(BLOCK + 4u * k);
    }
    tell_block("before the word is put back");
    REG(BLOCK + offset) = old;
    dump_bank_take(usage);
    uint32_t kept = 0;
    for (uint32_t k = 0; k < count; k++) {
        uint32_t i = (uint32_t)cosine16[k % 16u] & 0x3ffu, q = (uint32_t)cosine16[(k + 12u) % 16u] & 0x3ffu;
        kept += bank[k] == (i << 10 | q);
    }
    drv_say("dacprobe: word %x was %x, written %x, read %x at the end; %u changes in %u us; the pattern kept in %u of %u\n",
            offset, old, value, was[offset / 4u], changes, PROBE_US, kept, count);
}

void drv_run(struct drv *d, struct self *own)
{
    (void)own;
    uint32_t channel = config_number(d->conf, d->conf_len, "channel", 6);
    uint32_t write = config_number(d->conf, d->conf_len, "write", 0);
    uint32_t count = config_number(d->conf, d->conf_len, "count", 4096);
    uint32_t value = config_number(d->conf, d->conf_len, "value", 0);
    drv_must("a channel of 1 to 13", channel >= 1 && channel <= 13 ? ESP_OK : ESP_FAIL);
    drv_must("a count of 1 to COUNT_MAX", count >= 1 && count <= COUNT_MAX ? ESP_OK : ESP_FAIL);
    if (value == 0) {
        value = count | ENABLE;
    }
    /* words= in hexadecimal offsets, comma separated, the block's words no other engine's field is known in. */
    uint32_t words[WORDS_MAX] = { 0x0c, 0x10, 0x18, 0x1c, 0x00 }, n = 5;
    char list[64];
    list[0] = '\0';
    config_value(d->conf, d->conf_len, "words", list, sizeof(list));
    if (list[0] != '\0') {
        n = 0;
        for (const char *p = list; *p != '\0' && n < WORDS_MAX;) {
            uint32_t v = 0, digits = 0;
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
                p += 2;
            }
            for (;; p++, digits++) {
                char ch = *p >= 'A' && *p <= 'F' ? (char)(*p - 'A' + 'a') : *p;
                if (ch >= '0' && ch <= '9') {
                    v = v * 16u + (uint32_t)(ch - '0');
                } else if (ch >= 'a' && ch <= 'f') {
                    v = v * 16u + (uint32_t)(ch - 'a' + 10);
                } else {
                    break;
                }
            }
            drv_must("words= offsets in hexadecimal within the block, a word's each",
                     digits != 0 && v < 4u * BLOCK_WORDS && v % 4u == 0 ? ESP_OK : ESP_FAIL);
            words[n++] = v;
            if (*p == ',') {
                p++;
            } else {
                drv_must("words= offsets separated by commas alone", *p == '\0' ? ESP_OK : ESP_FAIL);
                break;
            }
        }
    }
    mac_up(d->mac);
    dump_known();
    up = 1;
    drv_must_mac("the channel", mac_channel(channel));
    d->channel = channel;
    child_report(&d->c, DRV_LISTENING);
    tell_block("the block as it is");
    if (write) {
        for (uint32_t k = 0; k < n; k++) {
            child_answer(&d->c);
            probe(words[k], value, count);
            tell_block("after the word is put back");
        }
    }
    for (;;) {
        uint32_t bits;
        child_answer(&d->c);
        if (d->leave) {
            break;
        }
        rv_wait(CHILD_INBOX, &bits);
    }
    drv_radio_off();
    drv_say("dacprobe: the radio is off\n");
    child_stop(&d->c, DRV_LEFT);
}
