#include "lib/self.h"

#include "lib/libc.h"

_Static_assert(ROOT_TABLE_SLOTS <= SELF_SLOTS, "the root task's table is larger than a self keeps account of");
_Static_assert(PROCESS_REGION_SLOTS_MAX <= 32, "regions are kept a bit each in a word");

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
    s->timer_line_count = TIMER_LINES;
    s->time = BOOT_CAP_TIME;
    s->print = BOOT_CAP_PRINT;
    s->slot_first = BOOT_CAP_COUNT;
    s->slot_end = ROOT_TABLE_SLOTS;
    s->regions = 0x3u; /* the boot installs the code in the first and the data in the second */
    TRY(s, "the root's region slots", rv_process_regions(s->process, &s->region_count, &s->region_most));

    uint32_t base, size, made;
    TRY(s, "the free RAM", rv_untyped_info(BOOT_CAP_FREE_RAM, &base, &size, &made));
    s->whole = (struct block){ BOOT_CAP_FREE_RAM, 0, base, size };
    s->free[s->free_count++] = s->whole;
    PASS(slot_new(s, &s->inbox));
    TRY(s, "an inbox", rv_pool_alloc(s->pool, CAP_NOTIFICATION, s->inbox, 0));
    /* Every core's units, as many as it keeps account of, from the first core's: the units are numbered core by core. */
    uint32_t time[3];
    TRY(s, "the units of time", rv_cap_info_of(s->time, CAP_TIME, time));
    s->time_first = time[0];
    s->time_count = time[1] < SELF_CORES * TIME_UNITS ? time[1] : SELF_CORES * TIME_UNITS;
    TRY(s, "keep the root's units", rv_time_bind(s->time, BOOT_CAP_THREAD, 0, units, 0));
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

uint32_t self_migrate(struct self *s, uint32_t pool, uint32_t slots)
{
    if (slots < s->slot_end || slots > SELF_SLOTS) {
        s->what = "a table no smaller than the one it has, nor larger than a self keeps account of";
        return KERR_INVALID_ARG;
    }
    uint32_t table, status;
    PASS(slot_new(s, &table));
    if ((status = rv_pool_alloc(pool, CAP_CAPTABLE, table, slots)) != KERR_OK ||
        (status = rv_process_mount(s->process, 1, table)) != KERR_OK) {
        s->what = "a new table, mounted second";
        slot_back(s, table);
        return status;
    }
    /*
     * Each capability goes to the same index of the new table, where SLOT_IN(1, i) names it meanwhile,
     * the new table's own capability last, as it is the one they move through.
     * An empty slot has nothing to move.
     */
    for (uint32_t i = 0; i < s->slot_end; i++) {
        status = i != table ? rv_cap_move(table, i, i) : KERR_INVALID_CAP;
        if (status != KERR_OK && status != KERR_INVALID_CAP) {
            s->what = "move a capability to the new table";
            return status;
        }
    }
    TRY(s, "move the new table's capability into it", rv_cap_move(table, table, table));
    /* Every number then names the new table again, first through the second table slot and then through the first. */
    TRY(s, "unmount the old table", rv_process_unmount(SLOT_IN(1, s->process), 0));
    TRY(s, "mount the new table first", rv_process_mount(SLOT_IN(1, s->process), 0, SLOT_IN(1, table)));
    TRY(s, "unmount the second table slot", rv_process_unmount(s->process, 1));
    /* What s->table holds names the old table: the new one's capability takes its place. */
    TRY(s, "delete the old table's capability", rv_cap_delete(table, s->table));
    TRY(s, "name the new table where the old one was named", rv_cap_move(table, s->table, table));
    PASS(slot_free(s, table));
    s->slot_end = slots;
    return KERR_OK;
}

uint32_t region_install(struct self *s, uint32_t frame, uint32_t rights, uint32_t *region)
{
    PASS(take_lowest(s, &s->regions, s->region_count, region, "a free region"));
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

uint32_t frame_zero(struct self *s, uint32_t frame, uint32_t base, uint32_t size)
{
    uint32_t region;
    PASS(region_install(s, frame, RIGHT_R | RIGHT_W, &region));
    memset((void *)(uintptr_t)base, 0, size);
    return region_free(s, region);
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
    for (uint32_t line = 0; line < s->timer_line_count; line++) {
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

uint32_t self_room(struct self *s, uint32_t size)
{
    if (size < SELF_ROOM_UNIT || size / SELF_ROOM_UNIT > 32u || (size & (size - 1u)) != 0) {
        s->what = "room of a power of two of units, 32 at most";
        return KERR_INVALID_ARG;
    }
    PASS(mem_frame(s, size, &s->room));
    PASS(region_install(s, s->room.made, RIGHT_R | RIGHT_W, &s->room_region));
    s->room_used = 0;
    return KERR_OK;
}

uint32_t self_room_free(struct self *s)
{
    if (s->room_used != 0) {
        s->what = "a room no child's data is in";
        return KERR_STATE;
    }
    PASS(region_free(s, s->room_region));
    PASS(mem_give(s, &s->room));
    s->room_region = 0;
    return KERR_OK;
}

/* A word's first n bits, n from 1 to 32. */
static uint32_t low_bits(uint32_t n)
{
    return n == 32u ? 0xffffffffu : (1u << n) - 1u;
}

uint32_t room_take(struct self *s, uint32_t size, struct block *b)
{
    uint32_t units = s->room.size / SELF_ROOM_UNIT, n = size / SELF_ROOM_UNIT;
    if (s->room.made == 0 || n == 0 || n * SELF_ROOM_UNIT != size || (n & (n - 1u)) != 0) {
        s->what = "room of a power of two of units";
        return KERR_INVALID_ARG;
    }
    for (uint32_t i = 0; i + n <= units; i += n) {
        if ((s->room_used >> i) & low_bits(n)) {
            continue;
        }
        uint32_t frame;
        PASS(slot_new(s, &frame));
        uint32_t status = rv_frame_carve(s->room.made, i * SELF_ROOM_UNIT, size, frame);
        if (status != KERR_OK) {
            s->what = "carve the room";
            slot_back(s, frame);
            return status;
        }
        s->room_used |= low_bits(n) << i;
        *b = (struct block){ 0, frame, s->room.base + i * SELF_ROOM_UNIT, size };
        return KERR_OK;
    }
    s->what = "free room for a child's data";
    return KERR_LIMIT;
}

void room_give(struct self *s, const struct block *b)
{
    uint32_t i = (b->base - s->room.base) / SELF_ROOM_UNIT;
    s->room_used &= ~(low_bits(b->size / SELF_ROOM_UNIT) << i);
}

/* The core of the unit at an offset in its time. */
static uint32_t unit_core(const struct self *s, uint32_t at)
{
    return (s->time_first + at) / TIME_UNITS;
}

uint32_t self_core(const struct self *s)
{
    return unit_core(s, 0);
}

uint32_t self_cores(const struct self *s)
{
    return s->time_count != 0 ? unit_core(s, s->time_count - 1) - unit_core(s, 0) + 1 : 1;
}

uint32_t core_unit(struct self *s, uint32_t core, uint32_t *unit)
{
    for (uint32_t at = 0; at < s->time_count; at++) {
        if (unit_core(s, at) == core) {
            *unit = at;
            return KERR_OK;
        }
    }
    s->what = "units of time of the core";
    return KERR_LIMIT;
}

uint32_t units_take(struct self *s, uint32_t core, uint32_t count, uint32_t *first)
{
    for (uint32_t at = 0; count > 0 && at + count <= s->time_count; at++) {
        if (unit_core(s, at) != core || unit_core(s, at + count - 1) != core) {
            continue;
        }
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
    s->what = "units of time of the core in a row";
    return KERR_LIMIT;
}

void units_give(struct self *s, uint32_t first, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        mark(s->units, first + i, 0);
    }
}

struct self_tally self_tally(const struct self *s)
{
    struct self_tally t = { mem_unused(s), s->slot_end - s->slot_first - slots_unused(s), s->regions, s->bits,
                            s->room_used, { 0 } };
    for (uint32_t i = 0; i < s->free_count; i++) {
        t.slots -= s->free[i].untyped >= s->slot_first && s->free[i].untyped < s->slot_end;
    }
    memcpy(t.units, s->units, sizeof(t.units));
    return t;
}
