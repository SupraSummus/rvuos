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
 * The kernel has no console, so after every record the driver carries
 * the log to the UART itself, by polling and with no system call,
 * and marks in the log's header what it has taken;
 * the halt writes out whatever the last record left.
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

/* The 16550 of QEMU virt, its transmitter polled. */
#define UART_THR 0
#define UART_LSR 5
#define UART_LSR_THRE 0x20u

static volatile uint8_t *uart;
static volatile struct rvuos_log *log_header;
static const volatile uint8_t *log_ring;
/* Bytes carried out so far. Only the thread that performed a record touches it, and it makes no call meanwhile. */
static uint32_t drained;

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

static void uart_putc(char c)
{
    if (c == '\n') {
        uart_putc('\r');
    }
    while ((uart[UART_LSR] & UART_LSR_THRE) == 0) {
    }
    uart[UART_THR] = (uint8_t)c;
}

static void drain_log(void)
{
    uint32_t head = log_header->head;
    for (; drained != head; drained++) {
        uart_putc((char)log_ring[drained % log_header->size]);
    }
    log_header->taken = head;
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
        drain_log();
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

    for (unsigned i = 0; i < REPLAY_AFTER_INPUT_COUNT; i++) {
        if (perform(&replay_after_input[i]) != KERR_OK) {
            puts("replay setup failed\n");
            rv_halt(BOOT_CAP_DEBUG, 2);
        }
    }
    uint32_t uart_base, log_base, size;
    if (rv_region_info(BOOT_CAP_UART, &uart_base, &size) != KERR_OK ||
        rv_region_info(BOOT_CAP_LOG, &log_base, &size) != KERR_OK) {
        puts("cannot find the uart or the log\n");
        rv_halt(BOOT_CAP_DEBUG, 2);
    }
    uart = (volatile uint8_t *)uart_base;
    log_header = (volatile struct rvuos_log *)log_base;
    log_ring = (const volatile uint8_t *)(log_base + RVUOS_LOG_HEADER);

    rv_invoke(OP_DEBUG_TRACE, BOOT_CAP_DEBUG, 0, 0, 0);
    if (perform(&replay_start) != KERR_OK) {
        puts("replay setup failed\n");
        rv_halt(BOOT_CAP_DEBUG, 2);
    }

    replay_loop(1);
    return 0;
}
