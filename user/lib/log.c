#include "lib/log.h"

#include "lib/self.h"

int logring_put(struct logring *l, char c)
{
    uint32_t head = l->head;
    l->bytes[head % LOGRING_SIZE] = c;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    l->head = head + 1u;
    return c == '\n';
}

uint32_t logring_take(const struct logring *l, uint32_t taken, void (*put)(char))
{
    uint32_t head = l->head;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (head - taken > LOGRING_SIZE) {
        taken = head - LOGRING_SIZE;
    }
    for (; taken != head; taken++) {
        put(l->bytes[taken % LOGRING_SIZE]);
    }
    return taken;
}

uint32_t kernel_log_open(struct self *s, struct kernel_log *k)
{
    uint32_t base, size, region;
    TRY(s, "the kernel's log", rv_frame_info(BOOT_CAP_LOG, &base, &size));
    PASS(region_install(s, BOOT_CAP_LOG, RIGHT_R | RIGHT_W, &region));
    k->header = (volatile struct rvuos_log *)(uintptr_t)base;
    k->ring = (const volatile uint8_t *)(uintptr_t)(base + RVUOS_LOG_HEADER);
    k->taken = k->header->taken;
    PASS(slot_new(s, &k->irq));
    TRY(s, "the log's line", rv_irq_carve(BOOT_CAP_IRQ_LINES, LOG_IRQ_LINE, 1, k->irq));
    TRY(s, "bind the log's line", rv_irq_bind(k->irq, s->pool, s->inbox, k->irq));
    return KERR_OK;
}

uint32_t kernel_log_arm(const struct kernel_log *k, uint32_t bit)
{
    return rv_irq_set(k->irq, bit);
}

void kernel_log_take(struct kernel_log *k, void (*put)(char))
{
    uint32_t head = k->header->head;
    uint32_t size = k->header->size;
    if (head - k->taken > size) {
        k->taken = head - size;
    }
    for (; k->taken != head; k->taken++) {
        put((char)k->ring[k->taken % size]);
    }
    k->header->taken = k->taken;
}
