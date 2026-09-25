/*
 * The kernel's log.
 * See DESIGN.md, "The kernel log".
 *
 * The kernel writes, one reader in user mode reads, and the kernel never waits:
 * a reader that falls more than KLOG_SIZE bytes behind loses the oldest.
 * The header is shared both ways: the kernel moves the head,
 * the reader moves `taken`, and the kernel trusts the latter no further
 * than the level of the log's line and what the halt writes out.
 */

#include "kernel.h"
#include "klog.h"
#include "object.h"

static volatile struct rvuos_log *header;
static volatile uint8_t *ring;

void klog_init(void)
{
    header = p2v(KLOG_BASE);
    ring = (volatile uint8_t *)p2v(KLOG_BASE + RVUOS_LOG_HEADER);
    header->head = 0;
    header->size = KLOG_SIZE;
    header->taken = 0;
}

/* Bytes written past what the reader has taken; a count past the head reads as the head. */
static uint32_t untaken(void)
{
    int32_t n = (int32_t)(header->head - header->taken);
    return n > 0 ? (uint32_t)n : 0;
}

bool klog_pending(void)
{
    return untaken() != 0;
}

void klog_append(char c)
{
    /*
     * An armed Irq means the reader had taken everything,
     * so only the byte that makes the line rise can find one to signal;
     * the rest of a burst is one look at the head cheaper.
     * Signalling on every byte would do no harm, but the trace lines after each call
     * would then hide a klog_set that ignores the level from the replay.
     */
    bool quiet = !klog_pending();
    ring[header->head % KLOG_SIZE] = (uint8_t)c;
    header->head++;
    if (quiet) {
        struct irq *irq = line_binding(LOG_IRQ_LINE);
        /*
         * The line is high and an Irq is armed on it: it hears once and is disarmed,
         * exactly as sched_interrupt treats a device's line.
         * There is no controller to mask; disarmed is masked.
         */
        if (irq != NULL && irq_armed(irq)) {
            irq_signal(irq);
        }
    }
}

void klog_set(struct irq *irq)
{
    /* The line is level: arming it while it is high is hearing it now. */
    if (irq_armed(irq) && klog_pending()) {
        irq_signal(irq);
    }
}

void klog_dump(void (*put)(char))
{
    uint32_t head = header->head;
    uint32_t n = untaken();
    if (n > KLOG_SIZE) {
        n = KLOG_SIZE;
    }
    for (uint32_t i = head - n; i != head; i++) {
        put((char)ring[i % KLOG_SIZE]);
    }
}

void (kputs)(const char *s)
{
    while (*s != '\0') {
        LOOP_ARG(kputs, s);
        kputc(*s++);
    }
}

void kput_hex(uint32_t v)
{
    static const char digits[] = "0123456789abcdef";
    kputs("0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        LOOP_BOUND(8);
        kputc(digits[(v >> shift) & 0xf]);
    }
}
