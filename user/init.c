/*
 * Root task for the current stage.
 * It walks the capability system once end to end on real hardware paths:
 * start a logger thread that carries the kernel's log to the UART
 * on the log's line and the transmitter's interrupt,
 * carve memory, make a pool, allocate from it,
 * build a second process out of nothing but capabilities,
 * exchange a word with it through shared memory and two notifications,
 * take turns with it through shared memory alone, which only the tick allows,
 * destroy a pool and watch both processes lose their capabilities to it,
 * lease the child memory through a derived capability and take it back by revoking,
 * lend the child memory it turns into a pool of its own
 * and take it back by destroying the child's pool,
 * sleep on a timer while nothing else can run,
 * bind and revoke an Irq on a line nothing drives,
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
    SLOT_POOL_REGION,
    SLOT_CHILD_DATA,
    SLOT_SHARED,
    SLOT_POOL,
    SLOT_CHILD_TABLE,
    SLOT_CHILD_PROCESS,
    SLOT_CHILD_THREAD,
    SLOT_UP,   /* the child signals, the root waits */
    SLOT_DOWN, /* the root signals, the child waits */
    SLOT_DOOMED_REGION, /* the memory of the pool that gets destroyed */
    SLOT_DOOMED_POOL,
    SLOT_DOOMED_NTFN,   /* a notification allocated from it */
    SLOT_STALE,         /* a second capability to that same notification */
    SLOT_NEW_POOL,      /* the pool built over the same memory afterwards */
    SLOT_NEW_NTFN,
    SLOT_LEASE,         /* memory leased to the child by derivation, and taken back by revoking */
    SLOT_LEASE_COPY,    /* a copy beside it, which the revoke leaves alone */
    SLOT_LENT,          /* memory lent to the child, which pools it */
    SLOT_LENT_POOL,     /* what the root task makes of it once it is back */
    SLOT_TIMER_NTFN,    /* what the timer signals, and the spare line's Irq as well */
    SLOT_TIMER,
    SLOT_SPARE_LINE,    /* a line nothing drives, carved out of the boot grant */
    SLOT_SPARE_LINE_COPY, /* a second capability to it, inert while the line is bound */
    SLOT_SPARE_IRQ,     /* the line bound to SLOT_TIMER_NTFN */
    SLOT_BOOT_NTFN,     /* a notification in the boot pool, for binding the line again */
    SLOT_SPARE_IRQ_AGAIN,
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
    CHILD_LENT,   /* memory the root task lends; the child's pool replaces it */
    CHILD_OWN_NTFN, /* allocated from that pool */
    CHILD_BOOT_POOL, /* the pool above the child's own; the child may not destroy it */
    CHILD_TABLE_SLOTS = 10,
};

/*
 * Region slots. The root task boots with code in 0 and data in 1.
 * Every region costs one PMP entry: the root task maps five and the child four.
 */
#define ROOT_SHARED_SLOT 2
#define ROOT_UART_SLOT 3
#define ROOT_LOG_SLOT 4
#define CHILD_CODE_SLOT 0
#define CHILD_DATA_SLOT 1
#define CHILD_SHARED_SLOT 2
#define CHILD_LEASE_SLOT 3

/* Offsets into the free RAM the root task was granted, each a multiple of a CHUNK-sized block. */
#define SHARED_OFFSET 0x0000u
#define DATA_OFFSET  0x1000u
#define POOL_OFFSET  0x2000u
#define DOOMED_OFFSET 0x3000u
#define LENT_OFFSET  0x4000u
#define LEASE_OFFSET 0x5000u
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

/* Whose turn it is in the handshake that uses no notification. */
#define TURN_CHILD 0x1u
#define TURN_ROOT  0x2u

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
 * What sleeping is on rvuos: arm a timer and wait on the notification it signals.
 * The same notification can carry a device's bit next to the timer's,
 * which makes a wait with a timeout the same two calls.
 */
static uint32_t sleep_us(uint32_t us)
{
    uint32_t bits;
    uint32_t status = rv_timer_set(SLOT_TIMER, BIT_TIMER, us);
    if (status != KERR_OK) {
        return status;
    }
    status = rv_wait(SLOT_TIMER_NTFN, &bits);
    if (status != KERR_OK) {
        return status;
    }
    return bits == BIT_TIMER ? KERR_OK : KERR_INVALID_ARG;
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
    if (rv_region_info(CHILD_SHARED, &base, &size) != KERR_OK) {
        rv_puts(CHILD_DEBUG, "child: no shared region\n");
        rv_halt(CHILD_DEBUG, 3);
    }

    volatile uint32_t *shared = (volatile uint32_t *)base;
    shared[0] = MAGIC;
    rv_signal(CHILD_UP, BIT_REQUEST);

    rv_wait(CHILD_DOWN, &bits);
    rv_puts(CHILD_DEBUG,
            bits == BIT_REPLY && shared[1] == MAGIC + 1 ? "child: reply ok\n"
                                                        : "child: reply FAILED\n");

    /* The other half of the root task's preemption check. */
    while (shared[2] != TURN_CHILD) {
    }
    shared[2] = TURN_ROOT;

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
     * The root task leased this process memory by deriving a capability into this table
     * and mapping the range here, then revoked below its own capability.
     * Both the derived capability and the mapping have to be gone;
     * the mapping is checked from the root task's side, since touching it here would fault.
     */
    rv_wait(CHILD_DOWN, &bits);
    rv_puts(CHILD_DEBUG,
            bits == BIT_LEASE && rv_region_info(CHILD_LEASE, &base, &size) == KERR_INVALID_CAP
                ? "child: lease revoked here too\n"
                : "child: lease revoked here too FAILED\n");
    rv_signal(CHILD_UP, BIT_LEASED);

    /*
     * The root task lends this process memory and it becomes a pool here,
     * below this process's own pool in the tree.
     * The root task then destroys that pool, and this one has to go with it,
     * or the memory would carry kernel objects nobody can reach.
     */
    rv_wait(CHILD_DOWN, &bits);
    rv_puts(CHILD_DEBUG,
            bits == BIT_POOL &&
                    rv_invoke(OP_REGION_TO_POOL, CHILD_LENT, CHILD_LENT, 0, 0) == KERR_OK &&
                    rv_invoke(OP_POOL_ALLOC, CHILD_LENT, CAP_NOTIFICATION, CHILD_OWN_NTFN, 0) ==
                        KERR_OK
                ? "child: pool made\n"
                : "child: pool made FAILED\n");
    /*
     * The pool this process lives in lies below the boot pool,
     * so destroying the boot pool would destroy this thread,
     * and a thread cannot destroy the pool it lives in.
     */
    rv_puts(CHILD_DEBUG,
            rv_invoke(OP_POOL_DESTROY, CHILD_BOOT_POOL, CHILD_BOOT_POOL, 0, 0) == KERR_STATE
                ? "child: cannot destroy the pool above\n"
                : "child: cannot destroy the pool above FAILED\n");
    rv_signal(CHILD_UP, BIT_POOLED);

    for (;;) {
        rv_wait(CHILD_DOWN, &bits);
    }
}

int main(void);

int main(void)
{
    uint32_t free_base, free_size, min_size, bits;
    uint32_t data_base, data_size, uart_base, uart_size, log_base, log_size;

    puts("hello from user mode\n");

    /*
     * The logger first, so that everything above and below reaches the UART
     * while the machine runs, and not only when the halt writes the log out.
     * Its objects come from the boot pool and its thread runs in this process,
     * on a stack in the middle of the data region; this thread's is at the top.
     */
    expect("uart info", rv_region_info(BOOT_CAP_UART, &uart_base, &uart_size));
    expect("log info", rv_region_info(BOOT_CAP_LOG, &log_base, &log_size));
    expect("data info", rv_region_info(BOOT_CAP_DATA, &data_base, &data_size));
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
    expect("start the logger", rv_invoke(OP_THREAD_RESUME, SLOT_LOGGER, 0, 0, 0));

    expect("free ram info", rv_region_info(BOOT_CAP_FREE_RAM, &free_base, &free_size));
    /*
     * Read through a4. Every region below is a page at an offset that is a multiple of one,
     * which carves as long as a page is no smaller than the smallest region.
     */
    expect("the layout fits the smallest region",
           rv_region_min_size(BOOT_CAP_FREE_RAM, &min_size) == KERR_OK && CHUNK % min_size == 0
               ? KERR_OK : KERR_INVALID_ARG);

    expect("carve pool region",
           rv_invoke(OP_REGION_CARVE, BOOT_CAP_FREE_RAM, POOL_OFFSET, CHUNK, SLOT_POOL_REGION));
    expect("carve the child's data region",
           rv_invoke(OP_REGION_CARVE, BOOT_CAP_FREE_RAM, DATA_OFFSET, CHUNK, SLOT_CHILD_DATA));
    expect("carve the shared region",
           rv_invoke(OP_REGION_CARVE, BOOT_CAP_FREE_RAM, SHARED_OFFSET, CHUNK, SLOT_SHARED));
    expect("region to pool",
           rv_invoke(OP_REGION_TO_POOL, SLOT_POOL_REGION, SLOT_POOL, 0, 0));

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
                     free_base + DATA_OFFSET + CHUNK, 0));
    expect("start the child",
           rv_invoke(OP_THREAD_RESUME, SLOT_CHILD_THREAD, 0, 0, 0));

    expect("map the shared region here",
           rv_invoke(OP_PROCESS_INSTALL, BOOT_CAP_PROCESS, ROOT_SHARED_SLOT, SLOT_SHARED,
                     RIGHT_R | RIGHT_W));
    volatile uint32_t *shared = (volatile uint32_t *)(free_base + SHARED_OFFSET);

    /* The child may or may not have run by now; the bits are sticky either way. */
    expect("wait for the child", rv_wait(SLOT_UP, &bits));
    expect("the child's word arrived",
           bits == BIT_REQUEST && shared[0] == MAGIC ? KERR_OK : KERR_INVALID_ARG);
    puts("root: message ok\n");

    shared[1] = MAGIC + 1;
    expect("answer the child", rv_signal(SLOT_DOWN, BIT_REPLY));

    /*
     * Preemption.
     * Each side spins on a word only the other writes and neither waits,
     * so only the tick gets them past this.
     */
    shared[2] = TURN_CHILD;
    while (shared[2] != TURN_ROOT) {
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
     * See DESIGN.md, "Kernel pools and revocation".
     */
    expect("carve the doomed region",
           rv_invoke(OP_REGION_CARVE, BOOT_CAP_FREE_RAM, DOOMED_OFFSET, CHUNK,
                     SLOT_DOOMED_REGION));
    expect("make it a pool",
           rv_invoke(OP_REGION_TO_POOL, SLOT_DOOMED_REGION, SLOT_DOOMED_POOL, 0, 0));
    expect("allocate a notification in it",
           rv_invoke(OP_POOL_ALLOC, SLOT_DOOMED_POOL, CAP_NOTIFICATION, SLOT_DOOMED_NTFN, 0));
    expect("copy that capability aside",
           rv_invoke(OP_CAP_COPY, BOOT_CAP_CAPTABLE, SLOT_STALE, SLOT_DOOMED_NTFN, RIGHT_ALL));
    expect("give the child one as well",
           rv_invoke(OP_CAP_COPY, SLOT_CHILD_TABLE, CHILD_DOOMED, SLOT_DOOMED_NTFN, RIGHT_ALL));
    expect("the notification works while its pool lives",
           rv_signal(SLOT_DOOMED_NTFN, BIT_REQUEST));

    /* REGION_TO_POOL cleared SLOT_DOOMED_REGION, so the memory comes back there. */
    expect("destroy the pool",
           rv_invoke(OP_POOL_DESTROY, SLOT_DOOMED_POOL, SLOT_DOOMED_REGION, 0, 0));
    expect("the copy is revoked",
           rv_signal(SLOT_STALE, BIT_REQUEST) == KERR_INVALID_CAP ? KERR_OK : KERR_INVALID_ARG);

    /*
     * The new notification lands at the address the old one had,
     * with the type the stale capability names.
     * Nothing in that slot tells the two objects apart,
     * so only the sweep at destroy time keeps the next line honest.
     */
    expect("build a pool over the same memory",
           rv_invoke(OP_REGION_TO_POOL, SLOT_DOOMED_REGION, SLOT_NEW_POOL, 0, 0));
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
     * Whether the child's mapping is gone shows in the overlap check:
     * memory installed anywhere cannot become a pool.
     */
    expect("carve memory to lease",
           rv_invoke(OP_REGION_CARVE, BOOT_CAP_FREE_RAM, LEASE_OFFSET, CHUNK, SLOT_LEASE));
    expect("copy it beside itself",
           rv_invoke(OP_CAP_COPY, BOOT_CAP_CAPTABLE, SLOT_LEASE_COPY, SLOT_LEASE, RIGHT_ALL));
    expect("derive it into the child's table",
           rv_invoke(OP_CAP_DERIVE, SLOT_CHILD_TABLE, CHILD_LEASE, SLOT_LEASE, RIGHT_R | RIGHT_W));
    expect("map it in the child",
           rv_invoke(OP_PROCESS_INSTALL, SLOT_CHILD_PROCESS, CHILD_LEASE_SLOT, SLOT_LEASE,
                     RIGHT_R | RIGHT_W));
    expect("the leased memory cannot be a pool while mapped",
           rv_invoke(OP_REGION_TO_POOL, SLOT_LEASE_COPY, SLOT_LENT_POOL, 0, 0) == KERR_OVERLAP
               ? KERR_OK : KERR_INVALID_ARG);
    expect("revoke below the lease",
           rv_invoke(OP_CAP_REVOKE, BOOT_CAP_CAPTABLE, SLOT_LEASE, 0, 0));
    uint32_t lease_base, lease_size;
    expect("the lease itself stays", rv_region_info(SLOT_LEASE, &lease_base, &lease_size));
    expect("the copy beside it stays", rv_region_info(SLOT_LEASE_COPY, &lease_base, &lease_size));
    expect("the child's mapping is gone",
           rv_invoke(OP_REGION_TO_POOL, SLOT_LEASE_COPY, SLOT_LENT_POOL, 0, 0));
    /* The memory comes back where the copy was: below the free RAM it was carved from. */
    expect("destroy that pool again",
           rv_invoke(OP_POOL_DESTROY, SLOT_LENT_POOL, SLOT_LEASE_COPY, 0, 0));
    expect("ask the child to look at the lease", rv_signal(SLOT_DOWN, BIT_LEASE));
    expect("wait for the child's answer", rv_wait(SLOT_UP, &bits));
    expect("the child looked at the lease", bits == BIT_LEASED ? KERR_OK : KERR_INVALID_ARG);
    puts("root: derivation ok\n");

    /*
     * Cascade.
     * The child turns lent memory into a pool of its own,
     * which makes the root task's capability to that memory inert.
     * Destroying the child's pool takes the child's own pool with it,
     * and the lent memory answers to the root task's capability again.
     * See DESIGN.md, "Kernel pools and revocation".
     */
    expect("carve memory to lend",
           rv_invoke(OP_REGION_CARVE, BOOT_CAP_FREE_RAM, LENT_OFFSET, CHUNK, SLOT_LENT));
    expect("lend it to the child",
           rv_invoke(OP_CAP_COPY, SLOT_CHILD_TABLE, CHILD_LENT, SLOT_LENT, RIGHT_ALL));
    expect("give the child the boot pool",
           rv_invoke(OP_CAP_COPY, SLOT_CHILD_TABLE, CHILD_BOOT_POOL, BOOT_CAP_POOL, RIGHT_ALL));
    expect("ask the child to pool it", rv_signal(SLOT_DOWN, BIT_POOL));
    expect("wait for the child's pool", rv_wait(SLOT_UP, &bits));
    expect("the child made a pool", bits == BIT_POOLED ? KERR_OK : KERR_INVALID_ARG);
    expect("the lent memory is inert here",
           rv_invoke(OP_REGION_TO_POOL, SLOT_LENT, SLOT_LENT_POOL, 0, 0) == KERR_OVERLAP
               ? KERR_OK : KERR_INVALID_ARG);

    /* SLOT_POOL_REGION was cleared when the pool was made; the memory comes back there. */
    expect("destroy the child's pool",
           rv_invoke(OP_POOL_DESTROY, SLOT_POOL, SLOT_POOL_REGION, 0, 0));
    expect("the lent memory is back",
           rv_invoke(OP_REGION_TO_POOL, SLOT_LENT, SLOT_LENT_POOL, 0, 0));
    puts("root: cascade ok\n");

    /*
     * Time.
     * The child is gone with its pool and the logger waits on the log,
     * which nothing but a running thread can feed,
     * so while this thread sleeps nothing is runnable
     * and the kernel has to wait for the tick rather than stop.
     */
    expect("allocate the timer's notification",
           rv_invoke(OP_POOL_ALLOC, SLOT_NEW_POOL, CAP_NOTIFICATION, SLOT_TIMER_NTFN, 0));
    expect("allocate the timer",
           rv_invoke(OP_POOL_ALLOC, SLOT_NEW_POOL, CAP_TIMER, SLOT_TIMER, SLOT_TIMER_NTFN));
    expect("sleep", sleep_us(SLEEP_US));
    expect("sleep again", sleep_us(SLEEP_US));
    puts("root: timer ok\n");

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
           rv_invoke(OP_POOL_DESTROY, SLOT_NEW_POOL, SLOT_DOOMED_REGION, 0, 0));
    expect("the irq is revoked",
           rv_irq_set(SLOT_SPARE_IRQ, BIT_SPARE) == KERR_INVALID_CAP ? KERR_OK : KERR_INVALID_ARG);
    expect("allocate a notification in the boot pool",
           rv_invoke(OP_POOL_ALLOC, BOOT_CAP_POOL, CAP_NOTIFICATION, SLOT_BOOT_NTFN, 0));
    expect("the line is free again",
           rv_invoke(OP_IRQ_BIND, SLOT_SPARE_LINE_COPY, BOOT_CAP_POOL, SLOT_BOOT_NTFN,
                     SLOT_SPARE_IRQ_AGAIN));
    puts("root: irq ok\n");

    expect("unmap the shared region",
           rv_invoke(OP_PROCESS_UNINSTALL, BOOT_CAP_PROCESS, ROOT_SHARED_SLOT, 0, 0));

    /*
     * The fault report lands in the log after the logger's last look at it,
     * so the halt writes it out; tests/run.sh reads it there.
     */
    puts("reading the removed region at ");
    put_hex((uint32_t)shared);
    puts(", expecting a fault\n");
    uint32_t word = shared[0];

    /* Not reached when PMP works. */
    puts("PMP did not stop the read\n");
    (void)word;
    rv_halt(BOOT_CAP_DEBUG, 2);
}
