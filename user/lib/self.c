#include "lib/self.h"

#include "lib/libc.h"

_Static_assert(ROOT_TABLE_SLOTS <= SELF_SLOTS, "the root task's table is larger than a self keeps account of");
_Static_assert(PROCESS_REGION_SLOTS <= 32, "regions are kept a bit each in a word");

static int taken(const uint32_t *map, uint32_t i)
{
    return (map[i / 32] >> (i % 32)) & 1u;
}

static void mark(uint32_t *map, uint32_t i, int used)
{
    if (used) {
        map[i / 32] |= 1u << (i % 32);
    } else {
        map[i / 32] &= ~(1u << (i % 32));
    }
}

uint32_t take_lowest(struct self *s, uint32_t *word, uint32_t count, uint32_t *i, const char *what)
{
    for (*i = 0; *i < count; (*i)++) {
        if (!(*word & (1u << *i))) {
            *word |= 1u << *i;
            return KERR_OK;
        }
    }
    s->what = what;
    return KERR_LIMIT;
}

uint32_t self_root(struct self *s, uint32_t units)
{
    memset(s, 0, sizeof(*s));
    s->table = BOOT_CAP_CAPTABLE;
    s->process = BOOT_CAP_PROCESS;
    s->pool = BOOT_CAP_POOL;
    s->code = BOOT_CAP_CODE;
    s->timer_lines = BOOT_CAP_TIMER_LINES;
    s->time = BOOT_CAP_TIME;
    s->debug = BOOT_CAP_DEBUG;
    s->slot_first = BOOT_CAP_COUNT;
    s->slot_end = ROOT_TABLE_SLOTS;
    s->regions = 0x3u; /* the boot installs the code in the first and the data in the second */

    uint32_t base, size, made;
    TRY(s, "the free RAM", rv_untyped_info(BOOT_CAP_FREE_RAM, &base, &size, &made));
    s->free[s->free_count++] = (struct block){ BOOT_CAP_FREE_RAM, 0, base, size };
    PASS(slot_new(s, &s->inbox));
    TRY(s, "an inbox", rv_pool_alloc(s->pool, CAP_NOTIFICATION, s->inbox, 0));
    TRY(s, "keep the root's units", rv_time_bind(s->time, BOOT_CAP_THREAD, 0, units));
    for (uint32_t i = 0; i < units; i++) {
        mark(s->units, i, 1);
    }
    return KERR_OK;
}

uint32_t slot_new(struct self *s, uint32_t *slot)
{
    for (uint32_t i = s->slot_first; i < s->slot_end; i++) {
        if (!taken(s->slots, i)) {
            mark(s->slots, i, 1);
            *slot = i;
            return KERR_OK;
        }
    }
    s->what = "a free slot";
    return KERR_LIMIT;
}

uint32_t slot_free(struct self *s, uint32_t slot)
{
    if (slot < s->slot_first || slot >= s->slot_end) {
        s->what = "a slot it did not hand out";
        return KERR_INVALID_ARG;
    }
    TRY(s, "delete a slot", rv_cap_delete(s->table, slot));
    mark(s->slots, slot, 0);
    return KERR_OK;
}

void slot_back(struct self *s, uint32_t slot)
{
    const char *what = s->what;
    slot_free(s, slot);
    s->what = what;
}

uint32_t slots_unused(const struct self *s)
{
    uint32_t n = 0;
    for (uint32_t i = s->slot_first; i < s->slot_end; i++) {
        n += !taken(s->slots, i);
    }
    return n;
}

uint32_t region_install(struct self *s, uint32_t frame, uint32_t rights, uint32_t *region)
{
    PASS(take_lowest(s, &s->regions, PROCESS_REGION_SLOTS, region, "a free region"));
    uint32_t status = rv_process_install(s->process, *region, frame, rights);
    if (status != KERR_OK) {
        s->regions &= ~(1u << *region);
        s->what = "install a frame";
    }
    return status;
}

uint32_t region_free(struct self *s, uint32_t region)
{
    TRY(s, "uninstall a frame", rv_process_uninstall(s->process, region));
    s->regions &= ~(1u << region);
    return KERR_OK;
}

uint32_t bit_new(struct self *s, uint32_t *bit)
{
    uint32_t i;
    PASS(take_lowest(s, &s->bits, 32, &i, "a free bit"));
    *bit = 1u << i;
    return KERR_OK;
}

void bit_free(struct self *s, uint32_t bit)
{
    s->bits &= ~bit;
}

uint32_t timer_bind(struct self *s, uint32_t pool, uint32_t ntfn, uint32_t *irq)
{
    PASS(slot_new(s, irq));
    for (uint32_t line = 0; line < TIMER_LINES; line++) {
        uint32_t status = rv_irq_carve(s->timer_lines, line, 1, *irq);
        if (status == KERR_OK) {
            /* The bind clears the line's slot, and may take it for the Irq. */
            status = rv_irq_bind(*irq, pool, ntfn, *irq);
            if (status == KERR_OK) {
                return KERR_OK;
            }
            rv_cap_delete(s->table, *irq);
        }
        if (status != KERR_OVERLAP) {
            s->what = "bind a timer line";
            slot_back(s, *irq);
            return status;
        }
    }
    s->what = "a timer line nothing is bound to";
    slot_back(s, *irq);
    return KERR_LIMIT;
}

uint32_t timer_new(struct self *s, uint32_t *irq)
{
    return timer_bind(s, s->pool, s->inbox, irq);
}

uint32_t units_take(struct self *s, uint32_t count, uint32_t *first)
{
    for (uint32_t at = 0; count > 0 && at + count <= TIME_UNITS; at++) {
        uint32_t n = 0;
        while (n < count && !taken(s->units, at + n)) {
            n++;
        }
        if (n == count) {
            for (uint32_t i = 0; i < count; i++) {
                mark(s->units, at + i, 1);
            }
            *first = at;
            return KERR_OK;
        }
    }
    s->what = "units of time in a row";
    return KERR_LIMIT;
}

void units_give(struct self *s, uint32_t first, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        mark(s->units, first + i, 0);
    }
}
