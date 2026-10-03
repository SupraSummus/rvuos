#ifndef RVUOS_USER_PHYTRACE_H
#define RVUOS_USER_PHYTRACE_H

/*
 * What the PHY tracer and the harness it traces, user/phyblob/, agree on; ESP32-C6 only.
 * make BOARD=esp32c6 phyblob builds the harness, and tools/esp32c6-run.py --ram loads it;
 * the tracer is still to come, see TODO.md.
 *
 * The harness is Espressif's PHY library, libphy.a, with a few lines of glue, at a fixed place in the free RAM.
 * It runs in a process that maps its own memory, the ROM and the ROM's RAM, and no device,
 * so every register access it makes faults and the tracer, its thread's watcher, carries it out.
 * It asks the tracer for anything else with an ebreak and the request in t0,
 * which no argument register carries, so a printf's arguments stay where the ABI put them.
 */

/*
 * The harness's block: code, data and stack, loaded by the ROM, read, written and run by the harness,
 * and read by the tracer, which decodes the harness's instructions and prints its strings.
 * It must end below 0x4086ad08, where the ROM keeps its buffers while it loads.
 */
#define PHYBLOB_BASE 0x40840000
#define PHYBLOB_SIZE 0x00010000

/*
 * The ROM's own data, bss and stack, which its PHY and libc functions use: ESP-IDF leaves
 * 0x4087c610 to the top of RAM to the ROM, and this is the block around that.
 */
#define PHYBLOB_ROM_RAM_BASE 0x4087c000
#define PHYBLOB_ROM_RAM_SIZE 0x00004000

/*
 * At PHYBLOB_BASE, so the tracer finds where to start the harness without its symbols.
 * The constants carry no suffix, since the harness's start.S and linker script read them too.
 */
#define PHYBLOB_MAGIC 0x50485954 /* "PHYT" */
#ifndef __ASSEMBLER__
#include <stdint.h>
struct phyblob_header {
    uint32_t magic;
    uint32_t entry;
    uint32_t stack_top;
};
#endif

/* Requests, in t0 at an ebreak; the tracer steps over the ebreak and resumes the harness but for PHYBLOB_DONE. */
#define PHYBLOB_DONE   0 /* a0 = what the harness returns; the trace ends */
#define PHYBLOB_MARK   1 /* a0 = a string naming the phase that starts */
#define PHYBLOB_PRINTF 2 /* a0 = a format, a1 on the arguments, as phy_printf got them */
#define PHYBLOB_DUMP   3 /* a0 = an address in the harness's block, a1 = a length in bytes */

#endif
