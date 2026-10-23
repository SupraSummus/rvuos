#ifndef RVUOS_WIFI_BRIDGE_H
#define RVUOS_WIFI_BRIDGE_H

/*
 * The bridge, a build with WIFI_BRIDGE=1: the host reads and writes the chip's backplane through the console,
 * at once with mode=bridge or none, or once mode=scan's scan, mode=sta's join or mode=ap's access point is up,
 * a line at a time, so that a program on the host, such as user/radio/cyw43439/tools/cyw43-rehost.py,
 * can stand where the chip's own cores stand.
 * The root task reads the host's lines and puts each ask in the driver's page,
 * and the driver answers it in the log, which the root task carries to the console:
 *
 *   r ADDR          the word at ADDR; "bridge: r ADDR = VALUE"
 *   R ADDR SPARE    the word at SPARE, then the word at ADDR, read again while it is the spare's, at most BRIDGE_READS
 *                   times; "bridge: R ADDR = VALUE READS"
 *   S ADDR SPARE    the two words at ADDR are sniffed from now on, each read after SPARE as R reads, 0 for none;
 *                   "bridge: S ADDR ok"
 *   w ADDR VALUE    VALUE written there; "bridge: w ADDR ok"
 *   W ADDR BYTES    up to BRIDGE_BYTES bytes, in hex, written there as one block; "bridge: W ADDR ok";
 *                   ADDR and the count of bytes multiples of four
 *   h ADDR          the halfword at ADDR, an even address; "bridge: h ADDR = VALUE"
 *   h ADDR VALUE    VALUE, of 16 bits, written there as a halfword; "bridge: h ADDR ok"; for a register of two halves,
 *                   as the 802.11 core's PHY's address and data are, which a word's write would both set
 *   end             the run is over
 *
 * Up to BRIDGE_WRITES writes, "w ADDR VALUE;", may come before an ask on its line, queued:
 * the bridge makes them first, and answers only the last ask, so that a write costs the host no wait of its own;
 * none goes in if any is refused.
 * After any ask but end, once S has named them, the two sniffed words are read and, if either is set, cleared,
 * and the answer ends " ! LO HI" with them, so that what the sniffed words collect reaches the host with every answer.
 * The page holds one ask, so the host asks again only once the last is answered;
 * a line that comes before that is answered "bridge: busy" and dropped.
 * Numbers are hex without 0x.
 * An ask the bridge refuses is answered "bridge: refused";
 * one it cannot read, or a line longer than the root task has room for, "bridge: what?".
 * Each ask is one access, an R that reads again aside, so a register a read changes is read once.
 * Where a read of the Bluetooth core's window, from BRIDGE_BT_WINDOW, gives the word of the read before,
 * as a slow register of the core's does, the host reads again itself,
 * or asks R with a word of the core's as the spare, which the bridge reads first:
 * a read that gives the spare's word gave the read before, and is made again.
 * The core's carrier word is written only within the band, as the vendor's code writes it:
 * its low seven bits 0 to 78 MHz above 2402, bit 7 set or not, nothing above, 0 among its values.
 * A halfword over it is refused too, being no whole word, and a block over it whatever it holds, and so is an S whose words cover it, since the bridge clears them;
 * so no ask of the host's takes the carrier out of the band.
 */

#include <stdint.h>

#include "cyw43.h"
#include "lib/say.h"

#define BRIDGE_BYTES     256u
#define BRIDGE_READS     4u
#define BRIDGE_WRITES    16u
#define BRIDGE_BT_WINDOW 0x19000000u
/* The Bluetooth core's carrier, 0x80 | (f - 2402 MHz). */
#define BRIDGE_BT_CARRIER (BRIDGE_BT_WINDOW + 0x003186a4u)

struct bridge_ask {
    uint32_t op; /* 'r', 'R', 'S', 'w', 'W', 'h', 'e' for end, or 0 for a line it could not read */
    uint32_t addr, value, len; /* value: what 'w' or 'h' writes, or where 'R' and 'S' read their spare; len: 'W''s bytes,
                                  or 2 for 'h' writing and 0 reading */
    uint8_t data[BRIDGE_BYTES];
    uint32_t writes; /* the queued writes before it, of write_value at write_at */
    uint32_t write_at[BRIDGE_WRITES], write_value[BRIDGE_WRITES];
};

/* What the driver keeps between asks: where S put the two sniffed words, and their spare; 0 for none. */
struct bridge_state {
    uint32_t sniff_at, sniff_spare;
};

/* The ask a line of the host's says; ask->op is 0 if it says none. */
void bridge_parse(const char *line, uint32_t len, struct bridge_ask *ask);
/* Does what the ask says on the chip, and answers it in out. */
void bridge_serve(struct cyw43 *chip, const struct bridge_ask *ask, struct bridge_state *st, const struct out *out);

#endif
