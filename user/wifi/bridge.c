/* The bridge's two halves: the root task's reading of a line, and the driver's serving of the ask; see bridge.h. */

#include "bridge.h"

#include "lib/libc.h"

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* A hex number at *at, past the spaces before it, into v; 0 if there is none or it is longer than a word. */
static int hex_word(const char *line, uint32_t len, uint32_t *at, uint32_t *v)
{
    uint32_t i = *at, n = 0;
    while (i < len && line[i] == ' ') { /* bounded: by the line's length */
        i++;
    }
    *v = 0;
    for (; i < len && hex_digit(line[i]) >= 0; i++, n++) { /* bounded: by the line's length */
        *v = *v << 4 | (uint32_t)hex_digit(line[i]);
    }
    *at = i;
    return n >= 1 && n <= 8;
}

/* One ask of a line, between its semicolons, into ask; ask->op is 0 if it says none. */
static void parse_one(const char *line, uint32_t len, struct bridge_ask *ask)
{
    uint32_t at = 1;
    ask->op = 0;
    if (len == 3 && memcmp(line, "end", 3) == 0) {
        ask->op = 'e';
        return;
    }
    if (len < 3 || line[1] != ' ' || !hex_word(line, len, &at, &ask->addr)) {
        return;
    }
    if (line[0] == 'r' && at == len) {
        ask->op = 'r';
    } else if ((line[0] == 'R' || line[0] == 'S') && hex_word(line, len, &at, &ask->value) && at == len) {
        ask->op = (uint32_t)line[0];
    } else if (line[0] == 'w' && hex_word(line, len, &at, &ask->value) && at == len) {
        ask->op = 'w';
    } else if (line[0] == 'W' && at < len && line[at] == ' ') {
        at++;
        uint32_t n = 0;
        for (; at + 1 < len && n < BRIDGE_BYTES; at += 2, n++) { /* bounded: BRIDGE_BYTES */
            int hi = hex_digit(line[at]), lo = hex_digit(line[at + 1]);
            if (hi < 0 || lo < 0) {
                return;
            }
            ask->data[n] = (uint8_t)(hi << 4 | lo);
        }
        if (at == len && n > 0 && (ask->addr & 3u) == 0 && (n & 3u) == 0) {
            ask->len = n;
            ask->op = 'W';
        }
    }
}

void bridge_parse(const char *line, uint32_t len, struct bridge_ask *ask)
{
    uint32_t start = 0;
    ask->writes = 0;
    for (uint32_t i = 0; i < len; i++) { /* bounded: by the line's length */
        if (line[i] != ';') {
            continue;
        }
        /* An ask before a semicolon is a write the bridge makes before the last ask, queued. */
        parse_one(line + start, i - start, ask);
        if (ask->op != 'w' || ask->writes == BRIDGE_WRITES) {
            ask->op = 0;
            return;
        }
        ask->write_at[ask->writes] = ask->addr;
        ask->write_value[ask->writes] = ask->value;
        ask->writes++;
        start = i + 1;
    }
    parse_one(line + start, len - start, ask);
    if (ask->op == 'e' && ask->writes != 0) {
        ask->op = 0;
    }
}

/*
 * Whether a write of len bytes at addr keeps the Bluetooth core's carrier in the band:
 * one clear of the carrier's word, or the word alone, value, within the band.
 */
static int bridge_allowed(uint32_t addr, uint32_t len, uint32_t value)
{
    if (addr >= BRIDGE_BT_CARRIER + 4u || (addr < BRIDGE_BT_CARRIER && len <= BRIDGE_BT_CARRIER - addr)) {
        return 1;
    }
    return addr == BRIDGE_BT_CARRIER && len == 4u && (value & ~0xffu) == 0 && (value & 0x7fu) <= 78u;
}

/* The word at addr, read after the one at spare, and again while it reads as the spare's; how many reads in *n. */
static uint32_t read_after(struct cyw43 *chip, uint32_t addr, uint32_t spare_at, uint32_t *n)
{
    uint32_t spare = cyw43_bp_read32(chip, spare_at);
    uint32_t v = cyw43_bp_read32(chip, addr);
    *n = 1;
    while (v == spare && *n < BRIDGE_READS) { /* bounded: BRIDGE_READS */
        v = cyw43_bp_read32(chip, addr);
        (*n)++;
    }
    return v;
}

/* The sniffed words, cleared once read; whether either was set. */
static int sniffed(struct cyw43 *chip, const struct bridge_state *st, uint32_t *lo, uint32_t *hi)
{
    uint32_t n;
    if (st->sniff_at == 0) {
        return 0;
    }
    *lo = read_after(chip, st->sniff_at, st->sniff_spare, &n);
    *hi = read_after(chip, st->sniff_at + 4u, st->sniff_spare, &n);
    if ((*lo | *hi) == 0) {
        return 0;
    }
    cyw43_bp_write32(chip, st->sniff_at, 0);
    cyw43_bp_write32(chip, st->sniff_at + 4u, 0);
    return 1;
}

void bridge_serve(struct cyw43 *chip, const struct bridge_ask *ask, struct bridge_state *st, const struct out *out)
{
    if (ask->op != 'r' && ask->op != 'R' && ask->op != 'S' && ask->op != 'w' && ask->op != 'W') {
        say(out, "bridge: what?\n");
        return;
    }
    /*
     * The queued writes go in only if every one of them, and the last ask, may.
     * A block over the carrier's word is refused whatever it holds, and so are sniffed words over it, which the bridge clears.
     */
    int allowed = ask->op == 'w' ? bridge_allowed(ask->addr, 4u, ask->value)
                  : ask->op == 'W' ? bridge_allowed(ask->addr, ask->len, ~0u)
                  : ask->op == 'S' ? bridge_allowed(ask->addr, 8u, ~0u) : 1;
    for (uint32_t i = 0; i < ask->writes; i++) { /* bounded: BRIDGE_WRITES */
        allowed = allowed && bridge_allowed(ask->write_at[i], 4u, ask->write_value[i]);
    }
    if (!allowed) {
        say(out, "bridge: refused\n");
        return;
    }
    for (uint32_t i = 0; i < ask->writes; i++) { /* bounded: BRIDGE_WRITES */
        cyw43_bp_write32(chip, ask->write_at[i], ask->write_value[i]);
    }
    uint32_t v = 0, n = 0, lo = 0, hi = 0;
    switch (ask->op) {
    case 'r':
        v = cyw43_bp_read32(chip, ask->addr);
        break;
    case 'R':
        v = read_after(chip, ask->addr, ask->value, &n);
        break;
    case 'S':
        st->sniff_at = ask->addr;
        st->sniff_spare = ask->value;
        break;
    case 'w':
        cyw43_bp_write32(chip, ask->addr, ask->value);
        break;
    case 'W':
        cyw43_load(chip, ask->addr, ask->data, ask->len);
        break;
    default:
        break;
    }
    /* One say a line, so that no other line of the log lands inside it. */
    const char *name = ask->op == 'S' ? "S" : ask->op == 'w' ? "w" : "W";
    if (!sniffed(chip, st, &lo, &hi)) {
        if (ask->op == 'r') {
            say(out, "bridge: r %x = %x\n", ask->addr, v);
        } else if (ask->op == 'R') {
            say(out, "bridge: R %x = %x %u\n", ask->addr, v, n);
        } else {
            say(out, "bridge: %s %x ok\n", name, ask->addr);
        }
    } else if (ask->op == 'r') {
        say(out, "bridge: r %x = %x ! %x %x\n", ask->addr, v, lo, hi);
    } else if (ask->op == 'R') {
        say(out, "bridge: R %x = %x %u ! %x %x\n", ask->addr, v, n, lo, hi);
    } else {
        say(out, "bridge: %s %x ok ! %x %x\n", name, ask->addr, lo, hi);
    }
}
