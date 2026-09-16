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
 * Two threads share the record cursor, see rvuos/replay.h,
 * so that a record which blocks one of them
 * leaves the kernel something to run,
 * and the blocking paths are replayed like any other.
 *
 * Records may do anything, including unmapping this program,
 * which then faults; the host build models that.
 * The driver ends with a breakpoint, which the kernel reports
 * as a user fault, so a finished run and a dead one
 * end the transcript with the same line on both sides.
 */

#include <stdint.h>

#include "rvuos.h"
#include "rvuos/replay.h"

static struct replay_record records[REPLAY_MAX_RECORDS];
static uint32_t count;

/*
 * The next record to perform.
 * A thread reads it and writes it back without an ecall in between,
 * and only an ecall can take the processor away,
 * so no atomic is needed. That changes when the timer arrives.
 */
static uint32_t cursor;

static void puts(const char *s)
{
    rv_puts(BOOT_CAP_DEBUG, s);
}

void replay_loop(void);
int main(void);

void replay_loop(void)
{
    for (;;) {
        uint32_t i = cursor;
        if (i >= count) {
            break;
        }
        cursor = i + 1;
        rv_invoke(records[i].op, records[i].slot, records[i].a1, records[i].a2, records[i].a3);
    }

    __asm__ volatile("ebreak");
    for (;;) {
    }
}

int main(void)
{
    for (unsigned i = 0; i < REPLAY_PROLOGUE_COUNT; i++) {
        const struct replay_record *r = &replay_prologue[i];
        uint32_t a1 = r->op == OP_THREAD_CONFIGURE ? (uint32_t)&replay_loop : r->a1;
        if (rv_invoke(r->op, r->slot, a1, r->a2, r->a3) != KERR_OK) {
            puts("replay setup failed\n");
            rv_halt(BOOT_CAP_DEBUG, 2);
        }
    }
    /*
     * The second thread is runnable from here on,
     * but it only runs once this one blocks, which takes a record,
     * so the records and the count below are in place before it looks.
     */

    uint32_t input_base, input_size;
    if (rv_region_info(BOOT_CAP_INPUT, &input_base, &input_size) != KERR_OK) {
        puts("cannot read the input region\n");
        rv_halt(BOOT_CAP_DEBUG, 2);
    }

    const volatile struct replay_header *hdr = (const volatile struct replay_header *)input_base;
    if (hdr->magic != REPLAY_MAGIC || hdr->count > REPLAY_MAX_RECORDS) {
        puts("bad replay header\n");
        rv_halt(BOOT_CAP_DEBUG, 2);
    }
    count = hdr->count;
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

    replay_loop();
    return 0;
}
