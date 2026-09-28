/*
 * Root task for the current stage.
 * It walks the capability system once end to end on real hardware paths:
 * start a logger thread that carries the kernel's log to the UART
 * on the log's line and the transmitter's interrupt,
 * halve untyped memory into frames and a pool, allocate from the pool,
 * build a second process out of nothing but capabilities,
 * exchange a word with it through shared memory and two notifications,
 * take turns with it through shared memory alone, which only the tick allows,
 * destroy a pool and watch both processes lose their capabilities to it,
 * lease the child a frame through a derived capability and take it back by revoking,
 * lend the child untyped memory it turns into a pool of its own
 * and take it back by revoking, which destroys that pool,
 * sleep on a timer while nothing else can run, and time it on the clock,
 * keep a period on the timer without drifting,
 * give a thread units of the processor that threads on spare time cannot take from it,
 * stop threads by revoking their units and start one again by binding it,
 * hold a thread to its units and let it run on spare time,
 * spin through the ticks while nothing else can run,
 * bind and revoke an Irq on a line nothing drives,
 * hand everything it holds to a successor in a process of its own,
 * which destroys the pool the root task lived in, and the root task with it,
 * then unmap a region and fault on it.
 * Negative paths are covered by the fuzz corpus and tests/differential.py.
 */

#include <stdint.h>

#include "console.h"
#include "rvuos.h"

/* Slots in the root task's own table, above the ones the kernel filled. */
enum {
    SLOT_LOG_NTFN = BOOT_CAP_COUNT, /* the logger waits here; the log's Irq and the UART's signal it */
    SLOT_LOG_LINE,      /* the log's line, carved out of the boot grant */
    SLOT_LOG_IRQ,       /* the line bound to SLOT_LOG_NTFN */
    SLOT_UART_LINE,     /* the UART's line */
    SLOT_UART_IRQ,
    SLOT_LOGGER,        /* the logger thread */
    SLOT_CHILD_DATA,
    SLOT_SHARED,
    SLOT_POOL,
    SLOT_CHILD_TABLE,
    SLOT_CHILD_PROCESS,
    SLOT_CHILD_THREAD,
    SLOT_UP,   /* the child signals, the root waits */
    SLOT_DOWN, /* the root signals, the child waits */
    SLOT_DOOMED_MEMORY, /* an Untyped of the size of the pool that gets destroyed */
    SLOT_DOOMED_POOL,
    SLOT_DOOMED_NTFN,   /* a notification allocated from it */
    SLOT_STALE,         /* a second capability to that same notification */
    SLOT_NEW_POOL,      /* the pool built over the same memory afterwards */
    SLOT_NEW_NTFN,
    SLOT_LEASE,         /* a frame leased to the child by derivation, and taken back by revoking */
    SLOT_LEASE_COPY,    /* a copy beside it, which the revoke leaves alone */
    SLOT_LENT,          /* an Untyped lent to the child, which pools it */
    SLOT_LENT_POOL,     /* what the root task makes of it once it is back */
    SLOT_TIMER_NTFN,    /* what the timer signals, and the spare line's Irq as well */
    SLOT_TIMER_LINE,    /* a timer line, carved out of the boot grant */
    SLOT_TIMER,         /* the timer line bound to SLOT_TIMER_NTFN */
    SLOT_SPARE_LINE,    /* a line nothing drives, carved out of the boot grant */
    SLOT_SPARE_LINE_COPY, /* a second capability to it, inert while the line is bound */
    SLOT_SPARE_IRQ,     /* the line bound to SLOT_TIMER_NTFN */
    SLOT_BOOT_NTFN,     /* a notification in the boot pool, for binding the line again */
    SLOT_SPARE_IRQ_AGAIN,
    SLOT_COUNTER,       /* the clock's frame, read only */
    SLOT_HALF_TIME,     /* the second half of the processor's units, the child's and then the spinners' */
    SLOT_CAPPED_TIME,   /* the same units without spare time, derived from those */
    SLOT_FREEZE_TIME,   /* every unit without spare time, to stop a thread for good */
    SLOT_EXIT_NTFN,     /* what the root task waits on once it has handed everything over */
    SLOT_SUCC_POOL,     /* the successor's pool, table, process and thread */
    SLOT_SUCC_TABLE,
    SLOT_SUCC_PROCESS,
    SLOT_SUCC_THREAD,
    SLOT_REST,          /* what is left of the free RAM, which the next block is taken from */
    SLOT_SPLIT,         /* its upper half, on its way to SLOT_REST */
    SLOT_BLOCK,         /* its lower half, on its way to becoming a frame or a pool */
    SLOT_SPINNER,       /* the spinners' threads, SPINNERS of them */
};

/*
 * Slots in the child's table.
 * Which capability sits where is a convention between the two of them
 * and means nothing to the kernel.
 */
enum {
    CHILD_DEBUG = 1,
    CHILD_SHARED,
    CHILD_UP,
    CHILD_DOWN,
    CHILD_DOOMED, /* a notification whose pool the root task destroys */
    CHILD_LEASE,  /* memory derived from the root task's, which revokes it */
    CHILD_LENT,   /* untyped memory the root task lends, derived from its own */
    CHILD_OWN_NTFN, /* allocated from the pool the child makes of it */
    CHILD_OWN_POOL, /* the pool the child lives in, which it may not destroy */
    CHILD_LENT_POOL, /* the pool the child makes of the lent memory */
    CHILD_TABLE_SLOTS = 11,
};

/*
 * Region slots. The root task boots with code in 0 and data in 1.
 * Every region costs one PMP entry: the root task maps six and the child four.
 */
#define ROOT_SHARED_SLOT 2
#define ROOT_UART_SLOT 3
#define ROOT_LOG_SLOT 4
#define ROOT_COUNTER_SLOT 5
#define CHILD_CODE_SLOT 0
#define CHILD_DATA_SLOT 1
#define CHILD_SHARED_SLOT 2
#define CHILD_LEASE_SLOT 3

/* The least block the root task takes, see take: the successor's pool holds a table as large as its own. */
#define CHUNK 0x1000u

/* The protocol between the two processes. */
#define BIT_REQUEST 0x1u
#define BIT_REPLY   0x2u
#define BIT_DONE    0x4u
#define BIT_CHECK   0x8u /* look at the capability whose pool is gone */
#define BIT_CHECKED 0x10u
#define BIT_POOL    0x20u /* turn the lent memory into a pool */
#define BIT_POOLED  0x40u
#define BIT_LEASE   0x200u /* look at the leased memory, which is gone */
#define BIT_LEASED  0x400u
#define BIT_TIMER   0x80u /* the timer's bit on its own notification */
#define BIT_SPARE   0x100u /* the spare line's bit on that same notification */
#define MAGIC 0x5eaf00du

/* The logger's notification: the log's bit and the UART's. */
#define BIT_LOG  0x1u
#define BIT_UART 0x2u

/*
 * The console device behind BOOT_CAP_UART, its line CONSOLE_IRQ
 * and a line nothing drives, SPARE_IRQ, are the board's; see console.h.
 */

/* Ticks are a millisecond on every board so far; long enough to need a few of them. */
#define SLEEP_US 10000u

/* A period, kept long enough that arming each from the wake would fall a period behind. */
#define PERIOD_US 5000u
#define PERIODS 20u

/* Whose turn it is in the handshake that uses no notification. */
#define TURN_CHILD 0x1u
#define TURN_ROOT  0x2u

/* The shared words: the child's message, the root task's answer, and the turn. */
#define SHARED_MESSAGE 0
#define SHARED_ANSWER  1
#define SHARED_TURN    2

/*
 * How the root task splits the processor's units: it keeps the first ROOT_UNITS,
 * the logger earns the next LOGGER_UNITS, and the child the half after them.
 */
#define ROOT_UNITS   28u
#define LOGGER_UNITS 4u
#define HALF_UNITS   (TIME_UNITS / 2)
_Static_assert(ROOT_UNITS + LOGGER_UNITS == HALF_UNITS, "the child's half follows the logger's units");

/* Threads the root task adds on the child's half once the child is done, each counting as it spins. */
#define SPINNERS 3u
_Static_assert(SLOT_SPINNER + SPINNERS <= ROOT_TABLE_SLOTS, "the root task's slots fit its table");
#define SPINNER_STACK 0x100u
#define SPIN_US 20000u
#define UNITS_US 40000u
#define CAPPED_US 80000u
static volatile uint32_t spins[SPINNERS];

static void puts(const char *s)
{
    rv_puts(BOOT_CAP_DEBUG, s);
}

static void put_hex(uint32_t v)
{
    puts("0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        rv_putc(BOOT_CAP_DEBUG, "0123456789abcdef"[(v >> shift) & 0xfu]);
    }
}

static void expect(const char *what, uint32_t status)
{
    puts(what);
    puts(status == KERR_OK ? ": ok\n" : ": FAILED\n");
    if (status != KERR_OK) {
        rv_halt(BOOT_CAP_DEBUG, 1);
    }
}

/*
 * The root task keeps no allocator.
 * It takes each block it needs off what is left of its free RAM:
 * the lower half, while the upper half is what is left from then on,
 * so the blocks halve one after another, and the last one is all that is left.
 * The Untyped it split goes each time, and what it made hangs right below the free RAM,
 * which holds on to every block.
 */
static uint32_t rest = BOOT_CAP_FREE_RAM;

/* The next block as an Untyped in slot: where it lies and how large it is, at least a page. */
static void take_untyped(uint32_t slot, uint32_t *base, uint32_t *size)
{
    uint32_t made;
    expect("halve what is left of the free ram", rv_split(rest, slot, SLOT_SPLIT));
    if (rest == SLOT_REST) {
        expect("let the halved one go", rv_invoke(OP_CAP_DELETE, BOOT_CAP_CAPTABLE, SLOT_REST, 0, 0));
    }
    expect("keep the upper half",
           rv_invoke(OP_CAP_MOVE, BOOT_CAP_CAPTABLE, SLOT_REST, SLOT_SPLIT, 0));
    rest = SLOT_REST;
    expect("the block holds a page",
           rv_untyped_info(slot, base, size, &made) == KERR_OK && *size >= CHUNK
               ? KERR_OK : KERR_INVALID_ARG);
}

/* The next block made into a frame or a pool in slot. */
static void take(uint32_t type, uint32_t slot, uint32_t *base, uint32_t *size)
{
    take_untyped(SLOT_BLOCK, base, size);
    expect("make the block", rv_retype(SLOT_BLOCK, type, slot, base));
    expect("let its Untyped go", rv_invoke(OP_CAP_DELETE, BOOT_CAP_CAPTABLE, SLOT_BLOCK, 0, 0));
}

/* The timer's bit, and it alone, on its notification. */
static uint32_t timer_wait(void)
{
    uint32_t bits;
    uint32_t status = rv_wait(SLOT_TIMER_NTFN, &bits);
    if (status != KERR_OK) {
        return status;
    }
    return bits == BIT_TIMER ? KERR_OK : KERR_INVALID_ARG;
}

/*
 * What sleeping is on rvuos: arm a timer line and wait on the notification it signals.
 * The same notification can carry a device's bit next to the timer's,
 * which makes a wait with a timeout the same two calls.
 */
static uint32_t sleep_us(uint32_t us)
{
    uint32_t status = rv_timer_set(SLOT_TIMER, BIT_TIMER, us);
    return status == KERR_OK ? timer_wait() : status;
}

static uint32_t period_wait(uint32_t us)
{
    uint32_t skipped;
    uint32_t status = rv_timer_period(SLOT_TIMER, BIT_TIMER, us, &skipped);
    return status == KERR_OK ? timer_wait() : status;
}

/*
 * The logger: the root task's second thread, and the only thing that writes the UART.
 * The kernel has no console; what it prints lands in its log, a ring behind BOOT_CAP_LOG
 * whose interrupt line is high while the ring holds bytes nobody has taken;
 * see DESIGN.md, "The kernel log".
 * So the logger drives two devices on one notification, one bit each:
 * the log says there is something to send and the UART says it can take a byte.
 * Which device the UART is, and when a byte has to wait for it, is the board's;
 * see console.h.
 * Its own failure is the one thing it cannot report through the log,
 * so that goes to the UART by polling.
 */
static volatile struct rvuos_log *log_header;
static const volatile uint8_t *log_ring;

static __attribute__((noreturn)) void logger_fail(const char *s)
{
    for (; *s != '\0'; s++) {
        if (*s == '\n') {
            console_put_polled('\r');
        }
        console_put_polled(*s);
    }
    console_flush();
    rv_halt(BOOT_CAP_DEBUG, 1);
}

/*
 * Wait for the UART's interrupt: arm its Irq and wait on the logger's notification.
 * The kernel masked the line as it signalled, so arming again is the acknowledgement.
 * The log's Irq is disarmed meanwhile, so the UART's bit comes alone.
 */
static void uart_wait(void)
{
    uint32_t bits;
    if (rv_irq_set(SLOT_UART_IRQ, BIT_UART) != KERR_OK ||
        rv_wait(SLOT_LOG_NTFN, &bits) != KERR_OK) {
        logger_fail("logger: cannot wait for the uart\n");
    }
    if (bits != BIT_UART) {
        logger_fail("logger: woken for something other than the uart\n");
    }
}

static void uart_put(char c)
{
    if (!console_put(c, uart_wait)) {
        logger_fail("logger: woken for something other than the transmitter\n");
    }
}

/* The log holds bare newlines; the terminal wants a carriage return before each. */
static void uart_put_line_ending(char c)
{
    if (c == '\n') {
        uart_put('\r');
    }
    uart_put(c);
}

static void uart_write(const char *s)
{
    for (; *s != '\0'; s++) {
        uart_put_line_ending(*s);
    }
}

static void logger_main(void)
{
    uint32_t taken = 0;

    console_start();
    for (;;) {
        /*
         * Say what has been taken so far, arm the log's line and wait.
         * The line is level, so bytes written before the arm wake the logger at once,
         * and bytes written after it wake it when they come.
         */
        uint32_t bits;
        log_header->taken = taken;
        if (rv_irq_set(SLOT_LOG_IRQ, BIT_LOG) != KERR_OK ||
            rv_wait(SLOT_LOG_NTFN, &bits) != KERR_OK) {
            logger_fail("logger: cannot wait for the log\n");
        }
        if (bits != BIT_LOG) {
            logger_fail("logger: woken for something other than the log\n");
        }

        uint32_t head = log_header->head;
        uint32_t size = log_header->size;
        if (head - taken > size) {
            /* The kernel wrote round the ring past this reader; the oldest byte kept is head - size. */
            taken = head - size;
            uart_write("logger: lost bytes\n");
        }
        for (; taken != head; taken++) {
            uart_put_line_ending((char)log_ring[taken % size]);
        }
        console_flush();
    }
}

/*
 * The child runs this, in its own process, out of the same code region.
 * It has no data region in common with the root task,
 * so it may touch no global: everything it needs is on its stack
 * or behind a capability the root task put in its table.
 */
static void child_main(void)
{
    uint32_t base, size, bits;

    rv_puts(CHILD_DEBUG, "child: started\n");
    if (rv_frame_info(CHILD_SHARED, &base, &size) != KERR_OK) {
        rv_puts(CHILD_DEBUG, "child: no shared region\n");
        rv_halt(CHILD_DEBUG, 3);
    }

    volatile uint32_t *shared = (volatile uint32_t *)base;
    shared[SHARED_MESSAGE] = MAGIC;
    rv_signal(CHILD_UP, BIT_REQUEST);

    rv_wait(CHILD_DOWN, &bits);
    rv_puts(CHILD_DEBUG,
            bits == BIT_REPLY && shared[SHARED_ANSWER] == MAGIC + 1 ? "child: reply ok\n"
                                                                    : "child: reply FAILED\n");

    /* The other half of the root task's preemption check. */
    while (shared[SHARED_TURN] != TURN_CHILD) {
    }
    shared[SHARED_TURN] = TURN_ROOT;

    rv_signal(CHILD_UP, BIT_DONE);

    /*
     * The root task destroys the pool the CHILD_DOOMED notification lives in
     * while this thread is stopped here.
     * Revocation has to reach this table, which belongs to another process
     * and which this thread never touches.
     */
    rv_wait(CHILD_DOWN, &bits);
    rv_puts(CHILD_DEBUG,
            bits == BIT_CHECK && rv_signal(CHILD_DOOMED, BIT_REQUEST) == KERR_INVALID_CAP
                ? "child: revoked here too\n"
                : "child: revoked here too FAILED\n");
    rv_signal(CHILD_UP, BIT_CHECKED);

    /*
     * The root task leased this process a frame by deriving a capability into this table
     * and mapping it here, then revoked below its own capability.
     * Both the derived capability and the mapping have to be gone;
     * the mapping is checked from the root task's side, since touching it here would fault.
     */
    rv_wait(CHILD_DOWN, &bits);
    rv_puts(CHILD_DEBUG,
            bits == BIT_LEASE && rv_frame_info(CHILD_LEASE, &base, &size) == KERR_INVALID_CAP
                ? "child: lease revoked here too\n"
                : "child: lease revoked here too FAILED\n");
    rv_signal(CHILD_UP, BIT_LEASED);

    /*
     * The root task lends this process untyped memory, derived from its own,
     * and it becomes a pool here, below the lent Untyped in the tree.
     * The root task then revokes below its own, and this pool has to go with it,
     * or the memory would carry kernel objects nobody can reach.
     */
    rv_wait(CHILD_DOWN, &bits);
    rv_puts(CHILD_DEBUG,
            bits == BIT_POOL &&
                    rv_retype(CHILD_LENT, CAP_POOL, CHILD_LENT_POOL, &base) == KERR_OK &&
                    rv_invoke(OP_POOL_ALLOC, CHILD_LENT_POOL, CAP_NOTIFICATION, CHILD_OWN_NTFN, 0) ==
                        KERR_OK
                ? "child: pool made\n"
                : "child: pool made FAILED\n");
    /* A thread keeps what it runs on: this one cannot destroy the pool it lives in. */
    rv_puts(CHILD_DEBUG,
            rv_invoke(OP_POOL_DESTROY, CHILD_OWN_POOL, 0, 0, 0) == KERR_STATE
                ? "child: cannot destroy its own pool\n"
                : "child: cannot destroy its own pool FAILED\n");
    rv_signal(CHILD_UP, BIT_POOLED);

    /* Nothing more is asked of it: it waits until its pool goes, and it with it. */
    for (;;) {
        rv_wait(CHILD_DOWN, &bits);
    }
}

/* The spinners: each counts in a word of its own and never waits. */
static void spin0(void)
{
    for (;;) {
        spins[0]++;
    }
}

static void spin1(void)
{
    for (;;) {
        spins[1]++;
    }
}

static void spin2(void)
{
    for (;;) {
        spins[2]++;
    }
}

static void (*const spinner_main[SPINNERS])(void) = { spin0, spin1, spin2 };

static uint32_t spun(void)
{
    uint32_t sum = 0;
    for (uint32_t i = 0; i < SPINNERS; i++) {
        sum += spins[i];
    }
    return sum;
}

/*
 * Read the counter's low word until it has moved on by counts,
 * and count the gaps between two reads longer than gap; *reads receives how many reads it took.
 */
static uint32_t spin_reads(uint32_t counter, uint32_t counts, uint32_t gap, uint32_t *reads)
{
    const volatile uint32_t *low = (const volatile uint32_t *)counter;
    uint32_t start = *low, last = start, now, gaps = 0, n = 0;
    do {
        now = *low;
        gaps += now - last > gap;
        last = now;
        n++;
    } while (now - start < counts);
    *reads = n;
    return gaps;
}

/*
 * Spin for ms milliseconds and count the traps that came meanwhile:
 * the gaps between two reads of the counter longer than eight reads take on average, plus a step of the counter.
 * A trap costs far more than a read on every board, while the counter's rate against the processor is the board's,
 * so the average comes from a first spin a tenth as long, which a trap would barely move.
 */
static uint32_t spin_traps(uint32_t counter, uint32_t hz, uint32_t ms)
{
    uint32_t counts = hz / 1000u * ms, reads;
    spin_reads(counter, counts / 10u, UINT32_MAX, &reads);
    return spin_reads(counter, counts, 8u * (counts / 10u) / reads + 2u, &reads);
}

/*
 * The successor, in a process and a table of its own,
 * into which the root task moved every capability it held, each to the same slot,
 * so every slot means here what it meant there; see DESIGN.md, "The root task is its capabilities".
 */
static __attribute__((noreturn)) void successor_main(void)
{
    uint32_t base, size;

    puts("successor: started\n");
    expect("stop the root task's thread for good",
           rv_invoke(OP_TIME_BIND, SLOT_FREEZE_TIME, BOOT_CAP_THREAD, 0, 0));
    /* The successor lives in a pool of its own, so nothing keeps it from destroying the boot pool. */
    expect("destroy the boot pool, and the root task with it",
           rv_invoke(OP_POOL_DESTROY, BOOT_CAP_POOL, 0, 0, 0));
    expect("the root task's thread is gone",
           rv_invoke(OP_THREAD_RESUME, BOOT_CAP_THREAD, 0, 0, 0) == KERR_INVALID_CAP
               ? KERR_OK : KERR_INVALID_ARG);
    /* The units the root task and the logger earned came free with them. */
    expect("earn the whole processor",
           rv_invoke(OP_TIME_BIND, BOOT_CAP_TIME, SLOT_SUCC_THREAD, 0, TIME_UNITS));
    /* The destroy emptied the root task's own slots; the successor's objects take them. */
    expect("take the root task's table slot",
           rv_invoke(OP_CAP_COPY, SLOT_SUCC_TABLE, BOOT_CAP_CAPTABLE, SLOT_SUCC_TABLE, RIGHT_ALL));
    expect("take the root task's process slot",
           rv_invoke(OP_CAP_COPY, SLOT_SUCC_TABLE, BOOT_CAP_PROCESS, SLOT_SUCC_PROCESS, RIGHT_ALL));
    expect("take the root task's thread slot",
           rv_invoke(OP_CAP_COPY, SLOT_SUCC_TABLE, BOOT_CAP_THREAD, SLOT_SUCC_THREAD, RIGHT_ALL));
    puts("root: handover ok\n");

    expect("shared region info", rv_frame_info(SLOT_SHARED, &base, &size));
    expect("map the shared region",
           rv_invoke(OP_PROCESS_INSTALL, BOOT_CAP_PROCESS, ROOT_SHARED_SLOT, SLOT_SHARED,
                     RIGHT_R | RIGHT_W));
    expect("unmap the shared region",
           rv_invoke(OP_PROCESS_UNINSTALL, BOOT_CAP_PROCESS, ROOT_SHARED_SLOT, 0, 0));

    /*
     * The fault report lands in the log after the logger's last look at it,
     * so the halt writes it out; tests/run.sh reads it there.
     */
    puts("reading the removed region at ");
    put_hex(base);
    puts(", expecting a fault\n");
    uint32_t word = *(volatile uint32_t *)base;

    /* Not reached when PMP works. */
    puts("PMP did not stop the read\n");
    (void)word;
    rv_halt(BOOT_CAP_DEBUG, 2);
}

/*
 * Hand the root task's place to a successor, which runs the same code on the same data, on a stack at sp,
 * and wait for good on a notification kept back.
 * The logger names its capabilities in this table, so it stops first,
 * and the halt writes out what it leaves in the log.
 */
static __attribute__((noreturn)) void hand_over(uint32_t sp)
{
    uint32_t base, bits;

    puts("root: handing over\n");
    expect("derive every unit without spare time",
           rv_invoke(OP_CAP_DERIVE, BOOT_CAP_CAPTABLE, SLOT_FREEZE_TIME, BOOT_CAP_TIME, RIGHT_W));
    expect("stop the logger", rv_invoke(OP_TIME_BIND, SLOT_FREEZE_TIME, SLOT_LOGGER, 0, 0));

    expect("make the successor's pool of what is left of the free ram",
           rv_retype(rest, CAP_POOL, SLOT_SUCC_POOL, &base));
    expect("allocate the successor's table",
           rv_invoke(OP_POOL_ALLOC, SLOT_SUCC_POOL, CAP_CAPTABLE, SLOT_SUCC_TABLE, ROOT_TABLE_SLOTS));
    expect("allocate the successor's process",
           rv_invoke(OP_POOL_ALLOC, SLOT_SUCC_POOL, CAP_PROCESS, SLOT_SUCC_PROCESS, SLOT_SUCC_TABLE));
    expect("allocate the successor's thread",
           rv_invoke(OP_POOL_ALLOC, SLOT_SUCC_POOL, CAP_THREAD, SLOT_SUCC_THREAD, SLOT_SUCC_PROCESS));
    expect("map the successor's code",
           rv_invoke(OP_PROCESS_INSTALL, SLOT_SUCC_PROCESS, 0, BOOT_CAP_CODE, RIGHT_R | RIGHT_X));
    expect("map the successor's data",
           rv_invoke(OP_PROCESS_INSTALL, SLOT_SUCC_PROCESS, 1, BOOT_CAP_DATA, RIGHT_R | RIGHT_W));
    expect("configure the successor",
           rv_invoke(OP_THREAD_CONFIGURE, SLOT_SUCC_THREAD, (uint32_t)&successor_main, sp, 0));
    /* On spare time, so it runs once this thread waits. */
    expect("bind the successor", rv_invoke(OP_TIME_BIND, BOOT_CAP_TIME, SLOT_SUCC_THREAD, 0, 0));
    expect("start the successor", rv_invoke(OP_THREAD_RESUME, SLOT_SUCC_THREAD, 0, 0, 0));
    expect("allocate what to wait on",
           rv_invoke(OP_POOL_ALLOC, BOOT_CAP_POOL, CAP_NOTIFICATION, SLOT_EXIT_NTFN, 0));

    /* From here on this thread holds no debug capability and says nothing; the empty slots fail. */
    for (uint32_t slot = BOOT_CAP_NULL + 1; slot < ROOT_TABLE_SLOTS; slot++) {
        if (slot != SLOT_SUCC_TABLE && slot != SLOT_EXIT_NTFN) {
            rv_invoke(OP_CAP_MOVE, SLOT_SUCC_TABLE, slot, slot, 0);
        }
    }
    rv_invoke(OP_CAP_MOVE, SLOT_SUCC_TABLE, SLOT_SUCC_TABLE, SLOT_SUCC_TABLE, 0);
    for (;;) {
        rv_wait(SLOT_EXIT_NTFN, &bits);
    }
}

int main(void);

int main(void)
{
    uint32_t free_base, free_size, free_made, min_size, bits;
    uint32_t shared_base, child_data_base, pool_base, doomed_base, new_pool_base, lease_base;
    uint32_t shared_size, child_data_size, pool_size, doomed_size, lease_size;
    uint32_t data_base, data_size, uart_base, uart_size, log_base, log_size;

    puts("hello from user mode\n");

    /*
     * The logger first, so that everything above and below reaches the UART
     * while the machine runs, and not only when the halt writes the log out.
     * Its objects come from the boot pool and its thread runs in this process,
     * on a stack in the middle of the data region; this thread's is at the top.
     */
    expect("uart info", rv_frame_info(BOOT_CAP_UART, &uart_base, &uart_size));
    expect("log info", rv_frame_info(BOOT_CAP_LOG, &log_base, &log_size));
    expect("data info", rv_frame_info(BOOT_CAP_DATA, &data_base, &data_size));
    expect("map the uart's registers",
           rv_invoke(OP_PROCESS_INSTALL, BOOT_CAP_PROCESS, ROOT_UART_SLOT, BOOT_CAP_UART,
                     RIGHT_R | RIGHT_W));
    expect("map the kernel's log",
           rv_invoke(OP_PROCESS_INSTALL, BOOT_CAP_PROCESS, ROOT_LOG_SLOT, BOOT_CAP_LOG,
                     RIGHT_R | RIGHT_W));
    console_init(uart_base);
    log_header = (volatile struct rvuos_log *)log_base;
    log_ring = (const volatile uint8_t *)(log_base + RVUOS_LOG_HEADER);

    expect("allocate the logger's notification",
           rv_invoke(OP_POOL_ALLOC, BOOT_CAP_POOL, CAP_NOTIFICATION, SLOT_LOG_NTFN, 0));
    expect("carve the log's line",
           rv_invoke(OP_IRQ_CARVE, BOOT_CAP_IRQ_LINES, LOG_IRQ_LINE, 1, SLOT_LOG_LINE));
    expect("bind the log's line",
           rv_invoke(OP_IRQ_BIND, SLOT_LOG_LINE, BOOT_CAP_POOL, SLOT_LOG_NTFN, SLOT_LOG_IRQ));
    expect("carve the uart's line",
           rv_invoke(OP_IRQ_CARVE, BOOT_CAP_IRQ_LINES, CONSOLE_IRQ, 1, SLOT_UART_LINE));
    expect("bind the uart's line",
           rv_invoke(OP_IRQ_BIND, SLOT_UART_LINE, BOOT_CAP_POOL, SLOT_LOG_NTFN, SLOT_UART_IRQ));
    expect("allocate the logger's thread",
           rv_invoke(OP_POOL_ALLOC, BOOT_CAP_POOL, CAP_THREAD, SLOT_LOGGER, BOOT_CAP_PROCESS));
    expect("configure the logger",
           rv_invoke(OP_THREAD_CONFIGURE, SLOT_LOGGER, (uint32_t)&logger_main,
                     data_base + data_size / 2, 0));
    /*
     * A thread runs only on units of the processor, and this one earns them all.
     * It keeps some and gives the logger a few, so that the logger has time of its own while others spin;
     * both are bound through the boot grant, and so run on spare time too.
     */
    expect("keep part of the processor",
           rv_invoke(OP_TIME_BIND, BOOT_CAP_TIME, BOOT_CAP_THREAD, 0, ROOT_UNITS));
    expect("give the logger units of its own",
           rv_invoke(OP_TIME_BIND, BOOT_CAP_TIME, SLOT_LOGGER, ROOT_UNITS, LOGGER_UNITS));
    expect("start the logger", rv_invoke(OP_THREAD_RESUME, SLOT_LOGGER, 0, 0, 0));

    expect("free ram info", rv_untyped_info(BOOT_CAP_FREE_RAM, &free_base, &free_size, &free_made));
    expect("nothing is made of the free ram yet", free_made == 0 ? KERR_OK : KERR_INVALID_ARG);
    /* Read through a4. Every block below holds a page, which is a block no smaller than the smallest region. */
    expect("the layout fits the smallest region",
           rv_frame_min_size(BOOT_CAP_DATA, &min_size) == KERR_OK && CHUNK % min_size == 0
               ? KERR_OK : KERR_INVALID_ARG);

    take(CAP_FRAME, SLOT_SHARED, &shared_base, &shared_size);
    take(CAP_FRAME, SLOT_CHILD_DATA, &child_data_base, &child_data_size);
    take(CAP_POOL, SLOT_POOL, &pool_base, &pool_size);
    expect("the blocks halve one after another",
           shared_base == free_base && shared_size == free_size / 2 &&
                   child_data_base == free_base + free_size / 2 && child_data_size == free_size / 4 &&
                   pool_base == child_data_base + child_data_size && pool_size == free_size / 8
               ? KERR_OK : KERR_INVALID_ARG);
    uint32_t none;
    expect("they hang below the free ram, which makes nothing more",
           rv_retype(BOOT_CAP_FREE_RAM, CAP_FRAME, SLOT_BLOCK, &none) == KERR_NO_MEMORY
               ? KERR_OK : KERR_INVALID_ARG);

    expect("allocate the child's table",
           rv_invoke(OP_POOL_ALLOC, SLOT_POOL, CAP_CAPTABLE, SLOT_CHILD_TABLE, CHILD_TABLE_SLOTS));
    expect("allocate the child's process",
           rv_invoke(OP_POOL_ALLOC, SLOT_POOL, CAP_PROCESS, SLOT_CHILD_PROCESS, SLOT_CHILD_TABLE));
    expect("allocate the child's thread",
           rv_invoke(OP_POOL_ALLOC, SLOT_POOL, CAP_THREAD, SLOT_CHILD_THREAD, SLOT_CHILD_PROCESS));
    expect("allocate the upward notification",
           rv_invoke(OP_POOL_ALLOC, SLOT_POOL, CAP_NOTIFICATION, SLOT_UP, 0));
    expect("allocate the downward notification",
           rv_invoke(OP_POOL_ALLOC, SLOT_POOL, CAP_NOTIFICATION, SLOT_DOWN, 0));

    /* The child executes the same flash as the root task, with its own data. */
    expect("map the child's code",
           rv_invoke(OP_PROCESS_INSTALL, SLOT_CHILD_PROCESS, CHILD_CODE_SLOT, BOOT_CAP_CODE,
                     RIGHT_R | RIGHT_X));
    expect("map the child's data",
           rv_invoke(OP_PROCESS_INSTALL, SLOT_CHILD_PROCESS, CHILD_DATA_SLOT, SLOT_CHILD_DATA,
                     RIGHT_R | RIGHT_W));
    expect("map the shared region there",
           rv_invoke(OP_PROCESS_INSTALL, SLOT_CHILD_PROCESS, CHILD_SHARED_SLOT, SLOT_SHARED,
                     RIGHT_R | RIGHT_W));

    /* Authority the child starts with, and nothing besides. */
    expect("give the child the log",
           rv_invoke(OP_CAP_COPY, SLOT_CHILD_TABLE, CHILD_DEBUG, BOOT_CAP_DEBUG, RIGHT_ALL));
    expect("give the child the shared region",
           rv_invoke(OP_CAP_COPY, SLOT_CHILD_TABLE, CHILD_SHARED, SLOT_SHARED, RIGHT_R | RIGHT_W));
    expect("give the child a way to signal",
           rv_invoke(OP_CAP_COPY, SLOT_CHILD_TABLE, CHILD_UP, SLOT_UP, RIGHT_W));
    expect("give the child a way to wait",
           rv_invoke(OP_CAP_COPY, SLOT_CHILD_TABLE, CHILD_DOWN, SLOT_DOWN, RIGHT_R));

    expect("configure the child's thread",
           rv_invoke(OP_THREAD_CONFIGURE, SLOT_CHILD_THREAD, (uint32_t)&child_main,
                     child_data_base + child_data_size, 0));
    /*
     * The child earns the other half of the processor, carved out of the boot grant
     * so that revoking below it stops the child alone.
     * Its account starts empty and fills in two ticks, and then this thread and the child both have time:
     * while both want the processor, they take turns.
     */
    expect("carve the child half the processor",
           rv_invoke(OP_TIME_CARVE, BOOT_CAP_TIME, HALF_UNITS, HALF_UNITS, SLOT_HALF_TIME));
    expect("bind the child to it",
           rv_invoke(OP_TIME_BIND, SLOT_HALF_TIME, SLOT_CHILD_THREAD, 0, HALF_UNITS));
    expect("start the child",
           rv_invoke(OP_THREAD_RESUME, SLOT_CHILD_THREAD, 0, 0, 0));

    expect("map the shared region here",
           rv_invoke(OP_PROCESS_INSTALL, BOOT_CAP_PROCESS, ROOT_SHARED_SLOT, SLOT_SHARED,
                     RIGHT_R | RIGHT_W));
    volatile uint32_t *shared = (volatile uint32_t *)shared_base;

    /* The child may or may not have run by now; the bits are sticky either way. */
    expect("wait for the child", rv_wait(SLOT_UP, &bits));
    expect("the child's word arrived",
           bits == BIT_REQUEST && shared[SHARED_MESSAGE] == MAGIC ? KERR_OK : KERR_INVALID_ARG);
    puts("root: message ok\n");

    shared[SHARED_ANSWER] = MAGIC + 1;
    expect("answer the child", rv_signal(SLOT_DOWN, BIT_REPLY));

    /*
     * Preemption.
     * Each side spins on a word only the other writes and neither waits,
     * so only the tick gets them past this.
     */
    shared[SHARED_TURN] = TURN_CHILD;
    while (shared[SHARED_TURN] != TURN_ROOT) {
    }
    puts("root: preemption ok\n");

    expect("wait for the child to finish", rv_wait(SLOT_UP, &bits));
    expect("the child is done", bits == BIT_DONE ? KERR_OK : KERR_INVALID_ARG);

    /*
     * Revocation.
     * A capability to an object in a destroyed pool must stay dead
     * when the same memory is rebuilt into the same kind of object.
     * A generation counter in the object could not promise that:
     * it dies with the memory it lives in and the rebuilt object
     * starts counting from one.
     * The pool is made of an Untyped of its own,
     * which is free again once the pool is gone and makes the same block again.
     * See DESIGN.md, "Kernel pools and revocation".
     */
    take_untyped(SLOT_DOOMED_MEMORY, &doomed_base, &doomed_size);
    expect("make it a pool",
           rv_retype(SLOT_DOOMED_MEMORY, CAP_POOL, SLOT_DOOMED_POOL, &doomed_base));
    expect("allocate a notification in it",
           rv_invoke(OP_POOL_ALLOC, SLOT_DOOMED_POOL, CAP_NOTIFICATION, SLOT_DOOMED_NTFN, 0));
    expect("copy that capability aside",
           rv_invoke(OP_CAP_COPY, BOOT_CAP_CAPTABLE, SLOT_STALE, SLOT_DOOMED_NTFN, RIGHT_ALL));
    expect("give the child one as well",
           rv_invoke(OP_CAP_COPY, SLOT_CHILD_TABLE, CHILD_DOOMED, SLOT_DOOMED_NTFN, RIGHT_ALL));
    expect("the notification works while its pool lives",
           rv_signal(SLOT_DOOMED_NTFN, BIT_REQUEST));

    /* The memory goes back to the Untyped the pool was made of. */
    expect("destroy the pool",
           rv_invoke(OP_POOL_DESTROY, SLOT_DOOMED_POOL, 0, 0, 0));
    expect("the copy is revoked",
           rv_signal(SLOT_STALE, BIT_REQUEST) == KERR_INVALID_CAP ? KERR_OK : KERR_INVALID_ARG);

    /*
     * The new notification lands at the address the old one had,
     * with the type the stale capability names.
     * Nothing in that slot tells the two objects apart,
     * so only the sweep at destroy time keeps the next line honest.
     */
    expect("build a pool over the same memory",
           rv_retype(SLOT_DOOMED_MEMORY, CAP_POOL, SLOT_NEW_POOL, &new_pool_base));
    expect("it is the same memory", new_pool_base == doomed_base ? KERR_OK : KERR_INVALID_ARG);
    expect("allocate a notification at the same address",
           rv_invoke(OP_POOL_ALLOC, SLOT_NEW_POOL, CAP_NOTIFICATION, SLOT_NEW_NTFN, 0));
    expect("the stale capability did not come back",
           rv_signal(SLOT_STALE, BIT_REQUEST) == KERR_INVALID_CAP ? KERR_OK : KERR_INVALID_ARG);
    puts("root: revocation ok\n");

    /* The sweep reached the child's table too, and the child never asked. */
    expect("ask the child to look", rv_signal(SLOT_DOWN, BIT_CHECK));
    expect("wait for the child's answer", rv_wait(SLOT_UP, &bits));
    expect("the child looked", bits == BIT_CHECKED ? KERR_OK : KERR_INVALID_ARG);

    /*
     * Derivation.
     * A capability derived from another is revoked with it,
     * wherever it went and whatever was installed from it,
     * while a copy made beside the original stays;
     * see DESIGN.md, "The derivation tree".
     * Whether the child's mapping is gone shows in the child's region slot,
     * which takes a region again once it is empty.
     */
    take(CAP_FRAME, SLOT_LEASE, &lease_base, &lease_size);
    expect("copy it beside itself",
           rv_invoke(OP_CAP_COPY, BOOT_CAP_CAPTABLE, SLOT_LEASE_COPY, SLOT_LEASE, RIGHT_ALL));
    expect("derive it into the child's table",
           rv_invoke(OP_CAP_DERIVE, SLOT_CHILD_TABLE, CHILD_LEASE, SLOT_LEASE, RIGHT_R | RIGHT_W));
    expect("map it in the child",
           rv_invoke(OP_PROCESS_INSTALL, SLOT_CHILD_PROCESS, CHILD_LEASE_SLOT, SLOT_LEASE,
                     RIGHT_R | RIGHT_W));
    expect("the child's slot is taken while it is mapped",
           rv_invoke(OP_PROCESS_INSTALL, SLOT_CHILD_PROCESS, CHILD_LEASE_SLOT, SLOT_LEASE_COPY,
                     RIGHT_R) == KERR_SLOT_IN_USE
               ? KERR_OK : KERR_INVALID_ARG);
    expect("revoke below the lease",
           rv_invoke(OP_CAP_REVOKE, BOOT_CAP_CAPTABLE, SLOT_LEASE, 0, 0));
    expect("the lease itself stays", rv_frame_info(SLOT_LEASE, &lease_base, &lease_size));
    expect("the copy beside it stays", rv_frame_info(SLOT_LEASE_COPY, &lease_base, &lease_size));
    expect("the child's mapping is gone",
           rv_invoke(OP_PROCESS_INSTALL, SLOT_CHILD_PROCESS, CHILD_LEASE_SLOT, SLOT_LEASE_COPY,
                     RIGHT_R));
    expect("unmap it again",
           rv_invoke(OP_PROCESS_UNINSTALL, SLOT_CHILD_PROCESS, CHILD_LEASE_SLOT, 0, 0));
    expect("ask the child to look at the lease", rv_signal(SLOT_DOWN, BIT_LEASE));
    expect("wait for the child's answer", rv_wait(SLOT_UP, &bits));
    expect("the child looked at the lease", bits == BIT_LEASED ? KERR_OK : KERR_INVALID_ARG);
    puts("root: derivation ok\n");

    /*
     * Cascade.
     * The child turns lent untyped memory into a pool of its own,
     * and the root task's Untyped makes nothing while the lent one lives.
     * Revoking below it destroys the child's pool,
     * and the root task's Untyped is free again, the whole of it.
     * See DESIGN.md, "Kernel pools and revocation".
     */
    uint32_t lent_base, lent_size;
    take_untyped(SLOT_LENT, &lent_base, &lent_size);
    expect("lend it to the child",
           rv_invoke(OP_CAP_DERIVE, SLOT_CHILD_TABLE, CHILD_LENT, SLOT_LENT, RIGHT_ALL));
    expect("give the child its own pool",
           rv_invoke(OP_CAP_COPY, SLOT_CHILD_TABLE, CHILD_OWN_POOL, SLOT_POOL, RIGHT_ALL));
    expect("ask the child to pool it", rv_signal(SLOT_DOWN, BIT_POOL));
    expect("wait for the child's pool", rv_wait(SLOT_UP, &bits));
    expect("the child made a pool", bits == BIT_POOLED ? KERR_OK : KERR_INVALID_ARG);
    uint32_t lent_again;
    expect("the lent memory is inert here",
           rv_retype(SLOT_LENT, CAP_POOL, SLOT_LENT_POOL, &lent_again) == KERR_NO_MEMORY
               ? KERR_OK : KERR_INVALID_ARG);
    expect("revoke below the lent memory",
           rv_invoke(OP_CAP_REVOKE, BOOT_CAP_CAPTABLE, SLOT_LENT, 0, 0));
    expect("the lent memory is back",
           rv_retype(SLOT_LENT, CAP_POOL, SLOT_LENT_POOL, &lent_again));
    expect("it is the same memory", lent_again == lent_base ? KERR_OK : KERR_INVALID_ARG);
    puts("root: cascade ok\n");

    /*
     * Time.
     * The child waits for the root task and the logger waits on the log,
     * which nothing but a running thread can feed,
     * so while this thread sleeps nothing is runnable
     * and the kernel has to wait for the tick rather than stop.
     */
    expect("allocate the timer's notification",
           rv_invoke(OP_POOL_ALLOC, SLOT_NEW_POOL, CAP_NOTIFICATION, SLOT_TIMER_NTFN, 0));
    expect("carve a timer line",
           rv_invoke(OP_IRQ_CARVE, BOOT_CAP_TIMER_LINES, 0, 1, SLOT_TIMER_LINE));
    expect("bind the timer line",
           rv_invoke(OP_IRQ_BIND, SLOT_TIMER_LINE, SLOT_NEW_POOL, SLOT_TIMER_NTFN, SLOT_TIMER));

    /*
     * The clock times the two sleeps.
     * A timer line fires no earlier than its delay, so the counter must show at least both;
     * how much more is the board's, so the transcript prints neither.
     */
    uint32_t hz, counter;
    expect("clock info", rv_clock_info(BOOT_CAP_CLOCK, &hz, &counter));
    expect("derive the counter's frame",
           rv_invoke(OP_CLOCK_FRAME, BOOT_CAP_CLOCK, SLOT_COUNTER, 0, 0));
    expect("map the counter",
           rv_invoke(OP_PROCESS_INSTALL, BOOT_CAP_PROCESS, ROOT_COUNTER_SLOT, SLOT_COUNTER, RIGHT_R));
    uint64_t start = rv_counter_read(counter);
    expect("sleep", sleep_us(SLEEP_US));
    expect("sleep again", sleep_us(SLEEP_US));
    uint64_t elapsed = rv_counter_read(counter) - start;
    puts("root: timer ok\n");
    expect("the clock saw both sleeps",
           elapsed * 1000000u >= (uint64_t)hz * (2 * SLEEP_US) ? KERR_OK : KERR_INVALID_ARG);
    puts("root: clock ok\n");

    /*
     * A period counts from the deadline before, not from the wake,
     * and the tick keeps pace with the counter,
     * so after the first wake sets the phase, PERIODS more take PERIODS periods on the clock,
     * give or take a wake's lateness.
     * A loop of sleeps would take a tick more per period.
     */
    expect("set the phase", period_wait(PERIOD_US));
    start = rv_counter_read(counter);
    uint32_t status = KERR_OK;
    for (uint32_t i = 0; i < PERIODS && status == KERR_OK; i++) {
        status = period_wait(PERIOD_US);
    }
    expect("wait the periods", status);
    elapsed = (rv_counter_read(counter) - start) * 1000000u;
    expect("the periods kept their pace",
           elapsed >= (uint64_t)hz * ((PERIODS - 1) * PERIOD_US)
                   && elapsed < (uint64_t)hz * ((PERIODS + 1) * PERIOD_US)
               ? KERR_OK : KERR_INVALID_ARG);
    puts("root: period ok\n");

    /*
     * Units; see DESIGN.md, "Scheduling".
     * The child is done, and its half goes to three spinners: the first earns a quarter of the processor
     * without spare time, and the other two earn nothing and run on spare time alone.
     * While this thread sleeps, the first runs about one tick in four, however many threads run on the rest,
     * and no more, so the other two spin about three times as far together.
     * The derived capability lies below the half, so revoking below the half stops all three.
     */
    expect("take the child's half back",
           rv_invoke(OP_CAP_REVOKE, BOOT_CAP_CAPTABLE, SLOT_HALF_TIME, 0, 0));
    expect("derive the half without spare time",
           rv_invoke(OP_CAP_DERIVE, BOOT_CAP_CAPTABLE, SLOT_CAPPED_TIME, SLOT_HALF_TIME, RIGHT_W));
    for (uint32_t i = 0; i < SPINNERS; i++) {
        expect("allocate a spinner",
               rv_invoke(OP_POOL_ALLOC, BOOT_CAP_POOL, CAP_THREAD, SLOT_SPINNER + i, BOOT_CAP_PROCESS));
        expect("configure it",
               rv_invoke(OP_THREAD_CONFIGURE, SLOT_SPINNER + i, (uint32_t)spinner_main[i],
                         data_base + data_size / 4 - i * SPINNER_STACK, 0));
        expect("bind it",
               i == 0 ? rv_invoke(OP_TIME_BIND, SLOT_CAPPED_TIME, SLOT_SPINNER, 0, TIME_UNITS / 4)
                      : rv_invoke(OP_TIME_BIND, SLOT_HALF_TIME, SLOT_SPINNER + i, 0, 0));
        expect("start it", rv_invoke(OP_THREAD_RESUME, SLOT_SPINNER + i, 0, 0, 0));
    }
    expect("let them spin", sleep_us(UNITS_US));
    /* Stopped before they are counted, since the tick would give them more turns meanwhile. */
    expect("take the spinners' units back",
           rv_invoke(OP_CAP_REVOKE, BOOT_CAP_CAPTABLE, SLOT_HALF_TIME, 0, 0));
    uint32_t root_spun = spun(), earned = spins[0], others = spins[1] + spins[2];
    for (uint32_t i = 0; i < SPINNERS; i++) {
        expect("every spinner had a turn", spins[i] != 0 ? KERR_OK : KERR_INVALID_ARG);
    }
    expect("the quarter ran a quarter of the time",
           2 * others > 3 * earned && others < 6 * earned ? KERR_OK : KERR_INVALID_ARG);
    puts("root: units ok\n");

    /* A thread bound to no units keeps its state and does not run, whoever else does. */
    expect("sleep while they have no units", sleep_us(SLEEP_US));
    expect("no thread without units ran", spun() == root_spun ? KERR_OK : KERR_INVALID_ARG);
    puts("root: unbind ok\n");

    /* Bound again, a thread goes on where it stopped: the first spinner, on spare time alone. */
    uint32_t first_spun = spins[0];
    expect("bind a spinner again",
           rv_invoke(OP_TIME_BIND, SLOT_HALF_TIME, SLOT_SPINNER, 0, 0));
    expect("sleep while it spins", sleep_us(SLEEP_US));
    expect("take its units back again",
           rv_invoke(OP_CAP_REVOKE, BOOT_CAP_CAPTABLE, SLOT_HALF_TIME, 0, 0));
    expect("it spun, and only it",
           spins[0] > first_spun && spun() - spins[0] == root_spun - first_spun ? KERR_OK : KERR_INVALID_ARG);
    puts("root: rebind ok\n");

    /*
     * Spare time; see DESIGN.md, "Scheduling".
     * Without spare time a thread runs no more than its units earn, however idle the processor is otherwise.
     * With an eighth and an account that starts empty, the spinner runs about 10 of the 81 ticks of the sleep,
     * one in eight, and the processor idles the rest;
     * with spare time it runs all of the next 41, since nothing else wants the processor.
     */
    expect("derive the half without spare time again",
           rv_invoke(OP_CAP_DERIVE, BOOT_CAP_CAPTABLE, SLOT_CAPPED_TIME, SLOT_HALF_TIME, RIGHT_W));
    first_spun = spins[0];
    expect("bind the spinner to an eighth without spare time",
           rv_invoke(OP_TIME_BIND, SLOT_CAPPED_TIME, SLOT_SPINNER, 0, TIME_UNITS / 8));
    expect("sleep while it earns its time", sleep_us(CAPPED_US));
    uint32_t capped = spins[0] - first_spun;
    expect("give it spare time",
           rv_invoke(OP_TIME_BIND, SLOT_HALF_TIME, SLOT_SPINNER, 0, TIME_UNITS / 8));
    first_spun = spins[0];
    expect("sleep while it runs on spare time", sleep_us(CAPPED_US / 2));
    uint32_t spare = spins[0] - first_spun;
    expect("take its units back for good",
           rv_invoke(OP_CAP_REVOKE, BOOT_CAP_CAPTABLE, SLOT_HALF_TIME, 0, 0));
    expect("it ran an eighth of the time, and all of the spare time",
           2 * capped < spare && 8 * capped > spare ? KERR_OK : KERR_INVALID_ARG);
    puts("root: spare ok\n");

    /*
     * The tick; see DESIGN.md, "Scheduling".
     * The timer interrupts only at a tick that could change what runs,
     * so a thread that spins while every other thread waits runs without a trap,
     * since it may go on with spare time whatever its account holds,
     * where a tick at every millisecond would end its turn only to hand it back.
     * It sleeps first so that the logger carries everything out and waits, and prints nothing as it spins.
     */
    status = sleep_us(SLEEP_US);
    uint32_t traps = spin_traps(counter, hz, SPIN_US / 1000u);
    expect("let the logger finish", status);
    expect("the only thread to run spun through the ticks",
           4 * traps < SPIN_US / 1000u ? KERR_OK : KERR_INVALID_ARG);
    puts("root: tickless ok\n");

    /* The child is done, and goes with its pool. */
    expect("destroy the child's pool", rv_invoke(OP_POOL_DESTROY, SLOT_POOL, 0, 0, 0));

    /*
     * Interrupts, beyond the two lines the logger lives on.
     * A line nothing drives shows what the logger cannot:
     * that a line is bound once, that an armed line nothing raises stays quiet,
     * and that the Irq dies with its pool, which frees the line.
     * It shares the timer's notification, one bit each,
     * which is what makes a wait with a timeout the same call as a wait.
     */
    expect("carve a spare line",
           rv_invoke(OP_IRQ_CARVE, BOOT_CAP_IRQ_LINES, SPARE_IRQ, 1, SLOT_SPARE_LINE));
    expect("carve it again",
           rv_invoke(OP_IRQ_CARVE, BOOT_CAP_IRQ_LINES, SPARE_IRQ, 1, SLOT_SPARE_LINE_COPY));
    expect("bind the line to the timer's notification",
           rv_invoke(OP_IRQ_BIND, SLOT_SPARE_LINE, SLOT_NEW_POOL, SLOT_TIMER_NTFN, SLOT_SPARE_IRQ));
    expect("the line is taken",
           rv_invoke(OP_IRQ_BIND, SLOT_SPARE_LINE_COPY, SLOT_NEW_POOL, SLOT_TIMER_NTFN,
                     SLOT_SPARE_IRQ_AGAIN) == KERR_OVERLAP
               ? KERR_OK : KERR_INVALID_ARG);
    expect("arm the line", rv_irq_set(SLOT_SPARE_IRQ, BIT_SPARE));
    /* Unmasked at the controller and never raised: the timer's bit comes alone. */
    expect("the idle line stays quiet", sleep_us(SLEEP_US));

    /* The Irq dies with its pool, which masks the line and frees it for the copy. */
    expect("destroy the driver's pool",
           rv_invoke(OP_POOL_DESTROY, SLOT_NEW_POOL, 0, 0, 0));
    expect("the irq is revoked",
           rv_irq_set(SLOT_SPARE_IRQ, BIT_SPARE) == KERR_INVALID_CAP ? KERR_OK : KERR_INVALID_ARG);
    expect("allocate a notification in the boot pool",
           rv_invoke(OP_POOL_ALLOC, BOOT_CAP_POOL, CAP_NOTIFICATION, SLOT_BOOT_NTFN, 0));
    expect("the line is free again",
           rv_invoke(OP_IRQ_BIND, SLOT_SPARE_LINE_COPY, BOOT_CAP_POOL, SLOT_BOOT_NTFN,
                     SLOT_SPARE_IRQ_AGAIN));
    puts("root: irq ok\n");

    hand_over(data_base + 3 * (data_size / 4));
}
