/*
 * EXPERIMENT: the CYW43439's Bluetooth core, brought up beside the running WLAN firmware
 * and taken over with a little code of our own in the core's RAM; see bt.h and BT.md.
 *
 * The core is brought up as embassy's bluetooth.rs does: the patch the firmware blob carries after its NVRAM
 * is written into the core's RAM over the backplane, the core is let go, and it is talked to over the HCI ring
 * the WLAN firmware keeps in that RAM. Polled, no interrupts.
 *
 * The core's own code lies in RAM (the ROM is read only), and the ROM branches into it at a few points.
 * One of them is taken over: our code in btcore/ runs the point's own first instruction, writes the poke,
 * serves the mailbox the host fills, and goes back into the patch, so our code runs on the core.
 */

#include "bt.h"

#include <stddef.h>

#include "btcore/bt-map.h" /* BT_POINT, BT_STUB, BT_MBOX, and the mailbox's words; see the code in btcore/ */
#include "cyw43.h"
#include "lib/libc.h"
#include "lib/say.h"
#include "wifi.h"

#ifdef WIFI_BT

#define BT_BASE       0x19000000u
#define BT_PWRUP      0x640894u
#define BT_CTRL       0x18000c7cu
#define BT_HOST_CTRL  0x18000d6cu
#define BT_RING_BASE  0x18000d68u
#define BT_FW_RDY     (1u << 24)
#define BT_AWAKE      (1u << 8)
#define BT_WAKE       (1u << 17)
#define BT_SW_RDY     (1u << 24)
#define BT_DATA_VALID (1u << 1)
#define BT_RING       0x1000u
#define BT_H2B_IN     0x2000u
#define BT_H2B_OUT    0x2004u
#define BT_B2H_IN     0x2008u
#define BT_B2H_OUT    0x200cu

/* The point, the stub and the mailbox are bt-map.h; these are the words of the core's the probe names. */
#define BT_SCRATCH 0x000d0da0u /* a word of the stub's own free RAM, between the code and the mailbox */
#define BT_TIMER   0x0032a004u /* the counter the firmware's own delay polls, about 1 MHz */
#define BT_MODEM   0x00318054u /* a modem register, written with its own value to time a write into the modem */
#define BT_CHAN    0x00318200u /* the channel index the firmware writes as it programs the modem */
#define BT_FREQ    0x003186a4u /* the carrier: 0x80 | (f - 2402 MHz) */
#define BT_AA      0x00318b70u /* the transmitter test's access address */

/* How long the observer's test waits for the firmware to settle, in ms; see the test in bt_run. */
#define BT_SETTLE 2000u

/* A word of the core's memory: its first read lags in the 0x19 window, so prime and take the next. */
static uint32_t bt_rd(struct bt_io *io, uint32_t addr)
{
    (void)cyw43_bp_read32(io->chip, BT_BASE + addr);
    return cyw43_bp_read32(io->chip, BT_BASE + addr);
}

/* Waits ms milliseconds a millisecond at a time, answering the root task's check on each. */
static void bt_turns(struct bt_io *io, uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t++) { /* bounded: by ms, which the callers fix */
        io->answer(io->answer_context);
        io->chip->sleep(io->chip, 1000);
    }
}

/* Asks the code on the core to run cmd with its two arguments; 0 and the result, or -1 if it never ran. */
static int bt_call(struct bt_io *io, uint32_t cmd, uint32_t arg0, uint32_t arg1, uint32_t *res)
{
    uint32_t before = bt_rd(io, BT_MBOX + MB_DONE);
    cyw43_bp_write32(io->chip, BT_BASE + BT_MBOX + MB_ARG0, arg0);
    cyw43_bp_write32(io->chip, BT_BASE + BT_MBOX + MB_ARG1, arg1);
    cyw43_bp_write32(io->chip, BT_BASE + BT_MBOX + MB_CMD, cmd);
    cyw43_bp_write32(io->chip, BT_BASE + BT_MBOX + MB_MAGIC, MB_ARMED);
    for (uint32_t i = 0; i < 500u; i++) { /* bounded: half a second at most */
        io->answer(io->answer_context);
        if (bt_rd(io, BT_MBOX + MB_DONE) != before) {
            *res = bt_rd(io, BT_MBOX + MB_RES);
            return 0;
        }
        io->chip->sleep(io->chip, 1000);
    }
    cyw43_bp_write32(io->chip, BT_BASE + BT_MBOX + MB_MAGIC, 0u); /* it never ran: disarm */
    return -1;
}

struct bt {
    uint32_t ring, h2b, b2h, host;
};

static uint8_t blob_byte(struct bt_io *io, uint32_t off)
{
    uint32_t left;
    return *io->blob(io->blob_context, off, &left);
}

static int bt_wait(struct bt_io *io, uint32_t bit, uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        if (cyw43_bp_read32(io->chip, BT_CTRL) & bit) {
            return 0;
        }
        io->chip->sleep(io->chip, 1000);
    }
    return -1;
}

/* Bytes to the backplane at any address, the words at either edge read first and kept. */
static void bt_write(struct bt_io *io, uint32_t addr, const uint8_t *p, uint32_t n)
{
    uint8_t buf[264];
    uint32_t start = addr & ~3u, pre = addr - start, total = (pre + n + 3u) & ~3u;
    uint32_t first = cyw43_bp_read32(io->chip, start), last = cyw43_bp_read32(io->chip, start + total - 4u);
    memcpy(buf, &first, 4);
    memcpy(buf + total - 4u, &last, 4);
    memcpy(buf + pre, p, n);
    cyw43_load(io->chip, start, buf, total);
}

/* The patch's records, as Intel HEX without the text: count, address, type, data. 0, or -1 for a malformed one. */
static int bt_upload(struct bt_io *io, uint32_t off, uint32_t size)
{
    uint32_t end = off + size, hi = 0, records = 0;
    off += blob_byte(io, off) + 2u;
    while (off + 4u <= end) { /* bounded: each record moves off forward by at least 4 */
        uint32_t n = blob_byte(io, off), addr = (uint32_t)blob_byte(io, off + 1u) << 8 | blob_byte(io, off + 2u);
        uint32_t type = blob_byte(io, off + 3u);
        uint8_t data[256];
        if (n == 0) {
            say(io->out, "bt: %u records written\n", records);
            return 0;
        }
        if (off + 4u + n > end) {
            return -1;
        }
        for (uint32_t i = 0; i < n; i++) {
            data[i] = blob_byte(io, off + 4u + i);
        }
        off += 4u + n;
        if (type == 4u) {
            hi = (uint32_t)data[0] << 8 | data[1];
        } else if (type == 0u) {
            bt_write(io, BT_BASE + (hi << 16) + addr, data, n);
            records++;
        }
    }
    return -1;
}

static void bt_host(struct bt_io *io, struct bt *b)
{
    cyw43_bp_write32(io->chip, BT_HOST_CTRL, b->host);
}

static void bt_cmd(struct bt_io *io, struct bt *b, uint16_t op, const uint8_t *par, uint32_t plen)
{
    uint8_t pkt[4 + 3 + 32];
    uint32_t len = 3u + plen, total = 4u + ((len + 3u) & ~3u);
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = (uint8_t)len;
    pkt[3] = 1; /* a command */
    pkt[4] = (uint8_t)op;
    pkt[5] = (uint8_t)(op >> 8);
    pkt[6] = (uint8_t)plen;
    memcpy(pkt + 7, par, plen);
    if (!(b->host & BT_WAKE)) {
        b->host |= BT_WAKE;
        bt_host(io, b);
    }
    if (bt_wait(io, BT_AWAKE, 300) != 0) {
        say(io->out, "bt: not awake for %x\n", op);
    }
    for (uint32_t i = 0; i < total; i += 4u) {
        uint32_t w;
        memcpy(&w, pkt + i, 4);
        cyw43_bp_write32(io->chip, b->ring + (b->h2b + i) % BT_RING, w);
    }
    b->h2b = (b->h2b + total) % BT_RING;
    cyw43_bp_write32(io->chip, b->ring + BT_H2B_IN, b->h2b);
    b->host ^= BT_DATA_VALID;
    bt_host(io, b);
}

/* Each event the controller sends within ms, in hex. */
static void bt_events(struct bt_io *io, struct bt *b, uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t++) {
        uint32_t in = cyw43_bp_read32(io->chip, b->ring + BT_B2H_IN);
        if (in == b->b2h) {
            io->chip->sleep(io->chip, 1000);
            continue;
        }
        uint32_t hdr = cyw43_bp_read32(io->chip, b->ring + BT_RING + b->b2h);
        uint32_t len = hdr & 0xffffffu, words = (len + 3u) / 4u;
        char line[2 * 64 + 1];
        uint32_t k = 0;
        for (uint32_t i = 0; i < words; i++) {
            uint32_t w = cyw43_bp_read32(io->chip, b->ring + BT_RING + (b->b2h + 4u + 4u * i) % BT_RING);
            for (uint32_t j = 0; j < 4u && 4u * i + j < len && k + 2u < sizeof(line); j++) {
                static const char hex[] = "0123456789abcdef";
                uint8_t c = (uint8_t)(w >> (8u * j));
                line[k++] = hex[c >> 4];
                line[k++] = hex[c & 15u];
            }
        }
        line[k] = '\0';
        b->b2h = (b->b2h + 4u + 4u * words) % BT_RING;
        cyw43_bp_write32(io->chip, b->ring + BT_B2H_OUT, b->b2h);
        say(io->out, "bt: type %u len %u: %s\n", hdr >> 24, len, line);
    }
}

/*
 * Our own code on the core, built from btcore/bt-stub.S and linked where the core sees it; see BT.md.
 * bt_takeover branches from BT_POINT to bt_code at BT_STUB, which runs that point's own first instruction,
 * writes the poke, serves the mailbox, and goes back into the patch; bt_measure is its timed run, at BT_MEASURE.
 */
extern const uint8_t bt_takeover[], bt_code[], bt_code_end[], bt_measure[], bt_measure_end[];

/*
 * Whether a poke is one we can vouch for keeps the carrier in the band, which is the only kind we arm.
 * The channel word takes an index 0..39; the frequency word takes 0x80 | (f - 2402 MHz), so its low seven bits
 * 0..78 (2402..2480 MHz) with bit 7 set and nothing above it; the access address takes any word. Any other
 * address is one we cannot vouch for, so a poke there is refused and none is armed.
 */
static int bt_poke_ok(uint32_t at, uint32_t val)
{
    if (at == BT_CHAN) {
        return val <= 39u;
    }
    if (at == BT_FREQ) {
        return (val & ~0xffu) == 0u && (val & 0x80u) != 0u && (val & 0x7fu) <= 78u;
    }
    if (at == BT_AA) {
        return 1;
    }
    return 0;
}

/*
 * Our code answers, and the core's timer and a write into the modem cost what they do: a word of our own RAM written
 * back, then runs of 64 and 128 writes timed by the firmware's own counter, which have to cost about twice the
 * ticks, and the counter over 50 ms of the host's, which gives its rate. Without those controls a count means nothing.
 * The counter counts down, so the stub's after-minus-before is the run's ticks negated.
 */
static void bt_selfcheck(struct bt_io *io)
{
    uint32_t got = 0, ticks = 0, t0 = 0, t1 = 0;
    int err = bt_call(io, MB_WRITE, BT_SCRATCH, 0xcafeu, &got);
    if (err == 0) {
        err = bt_call(io, MB_READ, BT_SCRATCH, 0u, &got);
    }
    say(io->out, "bt: our code answers %d, wrote cafe and read %x, runs %x\n", err, got, bt_rd(io, BT_MBOX + MB_RUNS));
    for (uint32_t n = 64u; n <= 128u; n *= 2u) { /* bounded: 2 */
        (void)bt_call(io, MB_WRITE, BT_SCRATCH, 0u, &got);
        err = bt_call(io, MB_MEASURE, BT_SCRATCH, n, &ticks);
        (void)bt_call(io, MB_READ, BT_SCRATCH, 0u, &got);
        say(io->out, "bt: %u writes into our RAM: call %d, %u ticks, the word %u\n", n, err, 0u - ticks, got);
        err = bt_call(io, MB_TOUCH, BT_MODEM, n, &ticks);
        say(io->out, "bt: %u writes into the modem: call %d, %u ticks\n", n, err, 0u - ticks);
    }
    (void)bt_call(io, MB_READ, BT_TIMER, 0u, &t0);
    io->chip->sleep(io->chip, 50000);
    (void)bt_call(io, MB_READ, BT_TIMER, 0u, &t1);
    say(io->out, "bt: the core's timer counted %u ticks over 50 ms\n", t0 - t1);
}

int bt_run(struct bt_io *io)
{
    uint32_t off = 0, size = 0, left;
    memcpy(&off, io->blob(io->blob_context, offsetof(struct blob_header, bt_offset), &left), 4);
    memcpy(&size, io->blob(io->blob_context, offsetof(struct blob_header, bt_size), &left), 4);
    if (size == 0u) {
        say(io->out, "bt: the blob carries no Bluetooth patch; tools/cyw43-blob.py --bt packs one\n");
        return -1;
    }
    if (io->channel > 39u) {
        say(io->out, "bt: channel %u refused: the test's channels are 0..39\n", io->channel);
        return -1;
    }
    struct bt b = { 0, 0, 0, 0 };
    say(io->out, "bt: patch at %u, %u bytes; ctrl %x\n", off, size, cyw43_bp_read32(io->chip, BT_CTRL));
    cyw43_bp_write32(io->chip, BT_BASE + BT_PWRUP, 3);
    io->chip->sleep(io->chip, 2000);
    if (bt_upload(io, off, size) != 0) {
        say(io->out, "bt: malformed patch\n");
        return -1;
    }
    /*
     * The branch over the point and our code both go in before the core is up: the point lies inside the vendor
     * patch, which takes a write only until FW_RDY. Each piece goes in as one block, the way the patch itself is.
     */
    bt_write(io, BT_BASE + BT_STUB, bt_code, (uint32_t)(bt_code_end - bt_code));
    bt_write(io, BT_BASE + BT_MEASURE, bt_measure, (uint32_t)(bt_measure_end - bt_measure));
    bt_write(io, BT_BASE + BT_POINT, bt_takeover, 4u);
    say(io->out, "bt: took %x with a stub at %x\n", BT_POINT, BT_STUB);
    if (bt_wait(io, BT_FW_RDY, 300) != 0) {
        say(io->out, "bt: never ready, ctrl %x\n", cyw43_bp_read32(io->chip, BT_CTRL));
        return -1;
    }
    b.ring = cyw43_bp_read32(io->chip, BT_RING_BASE);
    say(io->out, "bt: ready, ctrl %x, ring at %x\n", cyw43_bp_read32(io->chip, BT_CTRL), b.ring);
    /* Zero the mailbox before anything else, so the stub never serves the rubbish the RAM holds at a boot. */
    for (uint32_t mo = MB_MAGIC; mo <= MB_POKE_VAL; mo += 4u) { /* bounded: 9 */
        cyw43_bp_write32(io->chip, BT_BASE + BT_MBOX + mo, 0u);
    }
    for (uint32_t r = BT_H2B_IN; r <= BT_B2H_OUT; r += 4u) {
        cyw43_bp_write32(io->chip, b.ring + r, 0);
    }
    if (bt_wait(io, BT_AWAKE, 300) != 0) {
        say(io->out, "bt: never awake, ctrl %x\n", cyw43_bp_read32(io->chip, BT_CTRL));
        return -1;
    }
    b.host |= BT_SW_RDY;
    bt_host(io, &b);
    b.host ^= BT_DATA_VALID;
    bt_host(io, &b);
    bt_events(io, &b, 200);
    bt_cmd(io, &b, 0x0c03u, 0, 0); /* Reset */
    bt_events(io, &b, 300);
    bt_cmd(io, &b, 0x2003u, 0, 0); /* LE Read Local Supported Features */
    bt_events(io, &b, 200);
    /*
     * A word straight into the core's RAM before the test: the firmware reads some of the values it programs the
     * modem with out of RAM -- the test's access address at 0x20315c -- so writing the RAM makes the firmware write
     * our value itself, and no later write of its own undoes it.
     */
    if (io->mem != 0u) {
        cyw43_bp_write32(io->chip, BT_BASE + io->mem_at, io->mem);
        say(io->out, "bt: ram %x = %x\n", io->mem_at, io->mem);
    }
    /*
     * The poke, armed before the test: the firmware writes the modem's words itself on every call of the point, so
     * a one-time write from here is overwritten and says nothing; our code on the core writes it instead, on each
     * call, right after the point's own write, the last one in. Only a word we can vouch for keeps the carrier in
     * the band (bt_poke_ok), so anything else is refused and no typo can carry the carrier out of it.
     */
    if (io->poke != 0u && !bt_poke_ok(io->poke_at, io->poke)) {
        say(io->out, "bt: poke %x into %x refused: not a word known to stay in the band\n", io->poke, io->poke_at);
    } else if (io->poke != 0u) {
        cyw43_bp_write32(io->chip, BT_BASE + BT_MBOX + MB_POKE_AT, io->poke_at);
        cyw43_bp_write32(io->chip, BT_BASE + BT_MBOX + MB_POKE_VAL, io->poke);
        say(io->out, "bt: poke %x into %x on every call\n", io->poke, io->poke_at);
    }
    /*
     * The transmitter test, 37 bytes of PRBS9 on the channel asked for, 2402 + 2 * channel MHz. The point we took over
     * is reached as the firmware programs the modem for it, in a burst at its start, and not while the radio idles
     * or sends, so the mailbox is served right after the test starts. An observer on that channel, a BLE adapter in
     * its receiver test, hears the test with the firmware's access address and nothing with another.
     */
    /*
     * The test runs twice.
     * For its first second or so after the core comes up, the firmware passes the point for every packet it sends,
     * and our code there makes each packet late:
     * the observer hears none of them, or only some.
     * It hears all of them once the firmware has settled into passing the point once, as it sets the modem up,
     * and leaving the packets to the hardware.
     * Why it settles so is not known;
     * how long it takes was measured, between 1 and 1.5 s after LE Read Local Supported Features's answer.
     * So the first run is the self-check's, which needs the point to come round,
     * and the second, BT_SETTLE ms later, the observer's.
     */
    const uint8_t dtm[3] = { (uint8_t)io->channel, 0x25u, 0x00u };
    say(io->out, "bt: transmitter test on channel %u, for our code\n", io->channel);
    bt_cmd(io, &b, 0x201eu, dtm, 3);
    bt_turns(io, 200); /* just enough for the point to come round */
    bt_selfcheck(io);
    bt_cmd(io, &b, 0x201fu, 0, 0); /* LE Test End */
    bt_events(io, &b, 200);
    bt_turns(io, BT_SETTLE);
    say(io->out, "bt: transmitter test on channel %u, for the observer\n", io->channel);
    bt_cmd(io, &b, 0x201eu, dtm, 3);
    bt_turns(io, 900);
    bt_cmd(io, &b, 0x201fu, 0, 0);
    bt_events(io, &b, 200);
    say(io->out, "bt: test over, runs %x\n", bt_rd(io, BT_MBOX + MB_RUNS));
    return 0;
}

#endif /* WIFI_BT */
