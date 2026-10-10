/*
 * How nRF52840 halts.
 *
 * The board has no port of its own to the host: the probe reads RAM over SWD as the core runs,
 * the root task's console among it; see board.h.
 * So the halt only says that the machine halted, and with which code, in halt_record,
 * where tools/nrf52840-run.py finds it by its symbol;
 * the runner then writes out what the console held, and what no logger took from the log
 * under the line QEMU's halt writes, as RP2350's halt does, and exits with the code.
 * The record lies in the kernel's memory, which no thread reaches, so only the kernel says the machine halted.
 */

#include <stdint.h>

#include "kernel.h"
#include "layout.h"

/* "HALT" in the record's first word once the rest is written. */
#define HALT_MAGIC 0x544c4148u

struct halt_record {
    uint32_t magic;
    uint32_t code;
    uint32_t log;
};

volatile struct halt_record halt_record;

void khalt(int code)
{
    halt_record.code = (uint32_t)code;
    halt_record.log = KLOG_BASE;
    __asm__ volatile("dsb" : : : "memory");
    halt_record.magic = HALT_MAGIC;
    /* Every exception is held off while the kernel runs; the core waits here for the probe. */
    for (;;) {
        __asm__ volatile("wfi");
    }
}
