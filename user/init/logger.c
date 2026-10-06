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

#include <stdint.h>

#include "console.h"
#include "init.h"
#include "rvuos.h"

/* The logger's notification: the log's bit and the UART's. */
#define BIT_LOG  0x1u
#define BIT_UART 0x2u

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

/* Its objects come from the boot pool, and its thread runs in this process. */
void logger_start(uint32_t uart_base, uint32_t log_base, uint32_t sp)
{
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
    /* A bind takes the line's slot, so the Irq may go where the line was. */
    expect("carve the log's line",
           rv_invoke(OP_IRQ_CARVE, BOOT_CAP_IRQ_LINES, LOG_IRQ_LINE, 1, SLOT_LOG_IRQ));
    expect("bind the log's line",
           rv_invoke(OP_IRQ_BIND, SLOT_LOG_IRQ, BOOT_CAP_POOL, SLOT_LOG_NTFN, SLOT_LOG_IRQ));
    expect("carve the uart's line",
           rv_invoke(OP_IRQ_CARVE, BOOT_CAP_IRQ_LINES, CONSOLE_IRQ, 1, SLOT_UART_IRQ));
    expect("bind the uart's line",
           rv_invoke(OP_IRQ_BIND, SLOT_UART_IRQ, BOOT_CAP_POOL, SLOT_LOG_NTFN, SLOT_UART_IRQ));
    expect("allocate the logger's thread",
           rv_invoke(OP_POOL_ALLOC, BOOT_CAP_POOL, CAP_THREAD, SLOT_LOGGER, BOOT_CAP_PROCESS));
    expect("configure the logger",
           rv_invoke(OP_THREAD_CONFIGURE, SLOT_LOGGER, (uint32_t)&logger_main, sp, 0));
    /*
     * A thread runs only on units of the processor, and the root task's earns them all.
     * It keeps some and gives the logger a few, so that the logger has time of its own while others spin;
     * both are bound through the boot grant, and so run on spare time too.
     */
    expect("keep part of the processor",
           rv_invoke(OP_TIME_BIND, BOOT_CAP_TIME, BOOT_CAP_THREAD, 0, ROOT_UNITS));
    expect("give the logger units of its own",
           rv_invoke(OP_TIME_BIND, BOOT_CAP_TIME, SLOT_LOGGER, ROOT_UNITS, LOGGER_UNITS));
    expect("start the logger", rv_invoke(OP_THREAD_RESUME, SLOT_LOGGER, 0, 0, 0));
}
