#ifndef RVUOS_RADIO_DUMP_H
#define RVUOS_RADIO_DUMP_H

/*
 * The modem's dump engine, which copies what the receiver hears into a bank of SRAM, a word a sample,
 * up to 80 million a second, as Espressif's RF test library's adctrig drives it for the factory's tests;
 * ESPARGOS's ESP-SDR found it first, and hw/dump.md says what is known of it and whence.
 * A program that needs it, DRV_NEED_DUMP, has the bank, the modem and HP_SYSTEM mapped; prog/sampler.c is one.
 *
 * HP_SYSTEM's SRAM_USAGE hands the engine one of the four 128 KiB banks of SRAM, and the CPU loses the bank
 * meanwhile, so the bank is the free RAM's lower half, DUMP_BANK_BASE, taken whole by the root task before anything
 * else, and nothing else lies there: no kernel object, no stack, nothing a trap or another process may touch.
 * The children's memory lies in the free RAM's upper half instead; see drv.h.
 */

#include <stdint.h>

#include "drv.h"

#define DUMP_BANK      2u /* the bank's bit in SRAM_USAGE, one a bank from the first */

/*
 * What to capture, from the configuration's lines.
 * dump=, the samples a capture takes, 0 for none, at most the bank's words but one;
 * dumpsource=, the engine's source, 0 to 15, 15 the receiver's own I and Q, as adctrig takes it by default;
 * dumplane=, the first of the four lanes the word is made of, 0 for Wi-Fi's receiver, 14 for the BLE baseband's;
 * dumpbt=1, the BLE baseband's side, as adctrig's mode 8 sets it;
 * dumprate=, the engine's clock divided, 0 to 3, by 1, 2, 3 or 5 of 80 MHz;
 * dumpsweep=1, the source one more each capture, round the sixteen;
 * dumphex=, words of each capture told whole, at most DUMP_HEX_MAX, which user/radio/esp32c6/tools/dump-spectrum.py reads;
 * dumpmac=, in decimal, the Wi-Fi MAC's word 0x600A4E30 while source 5 is the engine's, as adctrig's seventh mode sets it,
 * 0 again after.
 */
struct dump_conf {
    uint32_t samples, source, lane, bt, rate, sweep, hex, mac;
};

#define DUMP_SAMPLES_MAX (DUMP_BANK_SIZE / 4u - 4u) /* four words past a capture tell whether it overran */
#define DUMP_HEX_MAX     8192u /* 0.64 s of the log's pace, so that a program still answers each second */

void dump_configure(const char *conf, uint32_t size, struct dump_conf *c);

/*
 * One capture and what it holds, in the log; the next one's source moves on if the configuration sweeps.
 * The kernel's log is a ring of 4 KiB that never waits for its reader, the root task, so dumphex='s words go out a few
 * lines at a time, with a pause between for the root task to carry them to the console; each line names its first
 * word's index, and user/radio/esp32c6/tools/dump-spectrum.py refuses a capture whose lines do not follow on.
 */
void dump_take(struct dump_conf *c);
/* The same in two: the capture alone, and then what it holds, so that what a capture was taken for may end between. */
void dump_capture(struct dump_conf *c);
void dump_tell(struct dump_conf *c);

/*
 * The engine writing round a ring of DUMP_RING words from the bank's first, as ESP-SDR found it does,
 * from dumpsource= at dumprate=, from dump_ring_start until dump_ring_stop, the bank the engine's meanwhile.
 * dump_ring_index reads the word it writes next, and bit 14 once the ring has come round.
 * A source a capture waits on to start stops the ring instead, on the same event, see hw/dump.md;
 * dump_ring_stopped says whether it has, and dump_ring_restart starts the ring over, its words marked again.
 * dump_ring_stop tells the ring's newest dumphex= words, the oldest first, as dump_take tells a capture.
 */
#define DUMP_RING 16384u
void dump_ring_start(const struct dump_conf *c);
uint32_t dump_ring_index(void);
int dump_ring_stopped(void);
void dump_ring_restart(const struct dump_conf *c);
void dump_ring_stop(const struct dump_conf *c);

/*
 * dump_arm arms a capture as dump_take does, but triggers it only if trigger, leaving its start to its source;
 * dump_disarm stops it, gives the bank back to the CPU, and says how many words the engine wrote.
 */
void dump_arm(const struct dump_conf *c, int trigger);
uint32_t dump_disarm(const struct dump_conf *c);

/*
 * The block as a program that drives its words itself may take it: the player stopped, to play from the bank's first word,
 * the block's clock 80 MHz, MAC_DUMP_ALLOC cleared, and the capture's source the receiver's own, 15, lanes 0 to 3,
 * the Wi-Fi side; the modem keeps each through a reset, so whatever an earlier run left, a halted one's too.
 */
void dump_known(void);

/*
 * The bank handed to the modem's engines, which the CPU then loses, their clock on, and SRAM_USAGE as it was;
 * dump_bank_take hands it back with that word. For an engine of the block's other than the capture's.
 */
uint32_t dump_bank_give(void);
void dump_bank_take(uint32_t usage);

#endif
