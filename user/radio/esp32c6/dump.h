#ifndef RVUOS_WIFI_DUMP_H
#define RVUOS_WIFI_DUMP_H

/*
 * The sampler of the Wi-Fi system on an ESP32-C6, behind C6_SDR=1: the root task's, not the driver's.
 * The modem holds a dump engine, which copies what the receiver hears into a bank of SRAM, a word a sample,
 * up to 80 million a second, as Espressif's RF test library's adctrig drives it for the factory's tests;
 * ESPARGOS's ESP-SDR found it first, and NOTES.md, "The dump engine", says what is known of it and whence.
 * While the radio listens, see drv.h, the root task takes a capture a second and tells what it holds.
 *
 * HP_SYSTEM's SRAM_USAGE hands the engine one of the four 128 KiB banks of SRAM, and the CPU loses the bank
 * meanwhile, so the bank is the free RAM's lower half, DUMP_BANK_BASE, taken whole before anything else,
 * and nothing else lies there: no kernel object, no stack, nothing a trap or another process may touch.
 * The children's memory lies in the free RAM's upper half instead; see drv.h.
 */

#include <stdint.h>

#include "lib/say.h"
#include "lib/self.h"

#define DUMP_BANK_BASE 0x40840000u
#define DUMP_BANK_SIZE 0x00020000u
#define DUMP_BANK      2u /* its bit in SRAM_USAGE, one a bank from the first */

/*
 * What to capture, from the configuration's lines; see root.c's configure.
 * dump=, the samples a capture takes, 0 for none, at most the bank's words but one;
 * dumpsource=, the engine's source, 0 to 15, 15 the receiver's own I and Q, as adctrig takes it by default;
 * dumplane=, the first of the four lanes the word is made of, 0 for Wi-Fi's receiver, 14 for the BLE baseband's;
 * dumpbt=1, the BLE baseband's side, as adctrig's mode 8 sets it;
 * dumprate=, the engine's clock divided, 0 to 3, by 1, 2, 3 or 5 of 80 MHz;
 * dumpsweep=1, the source one more each capture, round the sixteen;
 * dumphex=, words of each capture told whole, at most DUMP_HEX_MAX, which tools/dump-spectrum.py reads.
 */
struct dump_conf {
    uint32_t samples, source, lane, bt, rate, sweep, hex;
};

#define DUMP_SAMPLES_MAX (DUMP_BANK_SIZE / 4u - 4u) /* four words past a capture tell whether it overran */
#define DUMP_HEX_MAX     4096u

void dump_configure(const char *conf, uint32_t size, struct dump_conf *c);

/*
 * The bank, the modem and HP_SYSTEM, installed in regions of the root task's own;
 * bank is the block taken at DUMP_BANK_BASE, a frame not yet made.
 * What a capture holds goes to o, the kernel's log, and its words to raw, the console itself,
 * since tens of kilobytes a second through the log would be a call for every twelve bytes and would run it over;
 * drain hands the log to the console first, so that the words come after the lines before them.
 * must ends the run if a step failed, as the root task's own steps do.
 */
void dump_open(struct self *s, const struct out *o, const struct out *raw, struct block *bank, void (*drain)(void),
               void (*must)(const char *what, uint32_t status));

/* One capture and what it holds, in the log; the next one's source moves on if the configuration sweeps. */
void dump_take(struct dump_conf *c);

#endif
