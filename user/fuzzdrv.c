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
 * A record that names the other thread is passed to it as described there.
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
 * While tracing is on only an ecall can take the processor away,
 * and a thread makes none between reading the cursor and writing it back,
 * except the tick that passes a record on, after which it reads again;
 * so no atomic is needed. See rvuos/replay.h.
 */
static uint32_t cursor;

static void puts(const char *s)
{
    rv_puts(BOOT_CAP_DEBUG, s);
}

static uint32_t perform(const struct replay_record *r)
{
    return rv_invoke(r->op, r->slot, r->a1, r->a2, r->a3);
}

static void replay_loop(unsigned me);
void replay_second(void);
int main(void);

static void replay_loop(unsigned me)
{
    for (;;) {
        uint32_t i = cursor;
        if (i >= count) {
            break;
        }
        if (replay_passes(&records[i], me)) {
            perform(&replay_tick);
            if (cursor != i) {
                continue;
            }
            /* The round came back with the record untaken: its actor cannot run. */
        }
        cursor = i + 1;
        perform(&records[i]);
    }

    __asm__ volatile("ebreak");
    for (;;) {
    }
}

/* The second thread's entry point; the first thread is main. */
void replay_second(void)
{
    replay_loop(2);
}

int main(void)
{
    for (unsigned i = 0; i < REPLAY_PROLOGUE_COUNT; i++) {
        struct replay_record r = replay_prologue[i];
        if (r.op == OP_THREAD_CONFIGURE) {
            r.a1 = (uint32_t)&replay_second;
        }
        if (perform(&r) != KERR_OK) {
            puts("replay setup failed\n");
            rv_halt(BOOT_CAP_DEBUG, 2);
        }
    }

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
        records[i].actor = in[i].actor;
        records[i].slot = in[i].slot;
        records[i].a1 = in[i].a1;
        records[i].a2 = in[i].a2;
        records[i].a3 = in[i].a3;
    }

    rv_invoke(OP_DEBUG_TRACE, BOOT_CAP_DEBUG, 0, 0, 0);
    if (perform(&replay_start) != KERR_OK) {
        puts("replay setup failed\n");
        rv_halt(BOOT_CAP_DEBUG, 2);
    }

    replay_loop(1);
    return 0;
}
