/*
 * Replay driver.
 * The loader places a replay blob, see rvuos/abi.h,
 * in the region behind BOOT_CAP_INPUT.
 * The driver maps it, copies the records out,
 * turns on kernel tracing and self-checking,
 * and performs every recorded system call.
 * The kernel prints one line per call;
 * tests/differential.py compares them with the host build.
 *
 * Records may do anything, including unmapping this program,
 * which then faults; the host build models that.
 * The driver ends with a breakpoint, which the kernel reports
 * as a user fault, so a finished run and a dead one
 * end the transcript with the same line on both sides.
 */

#include <stdint.h>

#include "rvuos.h"

#define INPUT_SLOT 7

static struct replay_record records[REPLAY_MAX_RECORDS];

static void puts(const char *s)
{
    rv_puts(BOOT_CAP_DEBUG, s);
}

int main(void);

int main(void)
{
    uint32_t input_base, input_size;
    if (rv_region_info(BOOT_CAP_INPUT, &input_base, &input_size) != KERR_OK ||
        rv_invoke(OP_PROCESS_INSTALL, BOOT_CAP_PROCESS, INPUT_SLOT, BOOT_CAP_INPUT, RIGHT_R) != KERR_OK) {
        puts("cannot map the input region\n");
        rv_halt(BOOT_CAP_DEBUG, 2);
    }

    const volatile struct replay_header *hdr = (const volatile struct replay_header *)input_base;
    if (hdr->magic != REPLAY_MAGIC || hdr->count > REPLAY_MAX_RECORDS) {
        puts("bad replay header\n");
        rv_halt(BOOT_CAP_DEBUG, 2);
    }
    uint32_t count = hdr->count;
    const volatile struct replay_record *in =
        (const volatile struct replay_record *)(input_base + sizeof(*hdr));
    for (uint32_t i = 0; i < count; i++) {
        records[i].op = in[i].op;
        records[i].slot = in[i].slot;
        records[i].a1 = in[i].a1;
        records[i].a2 = in[i].a2;
        records[i].a3 = in[i].a3;
    }

    rv_invoke(OP_DEBUG_TRACE, BOOT_CAP_DEBUG, 0, 0, 0);

    for (uint32_t i = 0; i < count; i++) {
        rv_invoke(records[i].op, records[i].slot, records[i].a1, records[i].a2, records[i].a3);
    }

    __asm__ volatile("ebreak");
    for (;;) {
    }
}
