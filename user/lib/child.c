#include "lib/child.h"

#include "lib/libc.h"

/* A notification of the parent's, carved to bits, copied with RIGHT_W alone into slot dst of table. */
static uint32_t give_bits(struct self *s, uint32_t table, uint32_t dst, uint32_t ntfn, uint32_t bits)
{
    uint32_t carved;
    PASS(slot_new(s, &carved));
    uint32_t status = rv_notify_carve(ntfn, bits, carved);
    if (status == KERR_OK) {
        status = rv_cap_copy(table, dst, carved, RIGHT_W);
    }
    PASS(slot_free(s, carved));
    TRY(s, "give a child bits of a notification", status);
    return KERR_OK;
}

static uint32_t build(struct self *s, struct child *c, uint32_t table_slots, uint32_t data_size)
{
    if (table_slots <= CHILD_FIRST || table_slots > CHILD_TABLE_MAX) {
        s->what = "a child's table of a size that fits its pool";
        return KERR_INVALID_ARG;
    }
    c->table_slots = table_slots;
    c->slots_given = CHILD_FIRST;
    c->regions = (1u << CHILD_REGION_CODE) | (1u << CHILD_REGION_DATA);
    c->bits = CHILD_BIT_TIMER | CHILD_BIT_PARENT;

    c->in_room = s->room.made != 0;
    PASS(c->in_room ? room_take(s, data_size, &c->data) : mem_frame(s, data_size, &c->data));
    PASS(mem_take(s, CHILD_POOL_SIZE, &c->pool));
    PASS(mem_make(s, &c->pool, CAP_POOL));
    uint32_t pool = c->pool.made;
    PASS(slot_new(s, &c->table));
    TRY(s, "a child's table", rv_pool_alloc(pool, CAP_CAPTABLE, c->table, table_slots));
    PASS(slot_new(s, &c->process));
    TRY(s, "a child's process", rv_pool_alloc(pool, CAP_PROCESS, c->process, c->table));
    PASS(slot_new(s, &c->thread));
    TRY(s, "a child's thread", rv_pool_alloc(pool, CAP_THREAD, c->thread, c->process));
    PASS(slot_new(s, &c->inbox));
    TRY(s, "a child's inbox", rv_pool_alloc(pool, CAP_NOTIFICATION, c->inbox, 0));
    uint32_t timer;
    PASS(timer_bind(s, pool, c->inbox, &timer));
    uint32_t status = rv_cap_copy(c->table, CHILD_TIMER, timer, RIGHT_W);
    PASS(slot_free(s, timer));
    TRY(s, "give a child its timer", status);
    PASS(slot_free(s, pool));
    c->pool.made = 0;

    TRY(s, "install a child's code", rv_process_install(c->process, CHILD_REGION_CODE, s->code, RIGHT_R | RIGHT_X));
    TRY(s, "install a child's data",
        rv_process_install(c->process, CHILD_REGION_DATA, c->data.made, RIGHT_R | RIGHT_W));
    TRY(s, "give a child its inbox", rv_cap_copy(c->table, CHILD_INBOX, c->inbox, RIGHT_R));
    PASS(bit_new(s, &c->bit_page));
    PASS(bit_new(s, &c->bit_fault));
    PASS(give_bits(s, c->table, CHILD_PARENT, s->inbox, c->bit_page));
    TRY(s, "give a child its process", rv_cap_copy(c->table, CHILD_SELF, c->process, RIGHT_W));
    TRY(s, "give a child the log, to write", rv_cap_copy(c->table, CHILD_LOG, s->debug, RIGHT_W));

    if (c->in_room) {
        c->region = s->room_region;
    } else {
        PASS(region_install(s, c->data.made, RIGHT_R | RIGHT_W, &c->region));
    }
    c->page = (struct child_page *)(uintptr_t)c->data.base;
    PASS(slot_free(s, c->data.made));
    c->data.made = 0;
    /* A child starts from clean memory, whatever the block held before. */
    memset((void *)(uintptr_t)c->data.base, 0, c->data.size);
    c->page->state = CHILD_STARTING;
    return KERR_OK;
}

uint32_t child_new(struct self *s, struct child *c, const char *name, uint32_t table_slots, uint32_t data_size)
{
    memset(c, 0, sizeof(*c));
    c->name = name;
    uint32_t status = build(s, c, table_slots, data_size);
    if (status != KERR_OK) {
        const char *what = s->what;
        child_free(s, c);
        c->name = name;
        s->what = what;
    }
    return status;
}

uint32_t child_give(struct self *s, struct child *c, uint32_t slot, uint32_t rights, uint32_t *at)
{
    if (c->slots_given == c->table_slots) {
        s->what = "a free slot in a child's table";
        return KERR_LIMIT;
    }
    TRY(s, "give a child a capability", rv_cap_copy(c->table, c->slots_given, slot, rights));
    *at = c->slots_given++;
    return KERR_OK;
}

uint32_t child_give_bits(struct self *s, struct child *c, uint32_t ntfn, uint32_t bits, uint32_t *at)
{
    if (c->slots_given == c->table_slots) {
        s->what = "a free slot in a child's table";
        return KERR_LIMIT;
    }
    PASS(give_bits(s, c->table, c->slots_given, ntfn, bits));
    *at = c->slots_given++;
    return KERR_OK;
}

uint32_t child_slot(struct self *s, struct child *c, uint32_t *at)
{
    if (c->slots_given == c->table_slots) {
        s->what = "a free slot in a child's table";
        return KERR_LIMIT;
    }
    *at = c->slots_given++;
    return KERR_OK;
}

uint32_t child_put_bits(struct self *s, struct child *c, uint32_t at, uint32_t ntfn, uint32_t bits)
{
    return give_bits(s, c->table, at, ntfn, bits);
}

uint32_t child_code(struct self *s, struct child *c, uint32_t frame)
{
    TRY(s, "uninstall a child's code", rv_process_uninstall(c->process, CHILD_REGION_CODE));
    TRY(s, "install a child's own code", rv_process_install(c->process, CHILD_REGION_CODE, frame, RIGHT_R | RIGHT_X));
    return KERR_OK;
}

/*
 * What a carve put into the parent's slot carved, given into the child's next free slot,
 * or the carve's failure, status, passed on; the parent's slot goes either way.
 */
static uint32_t give_carved(struct self *s, struct child *c, uint32_t carved, uint32_t status, uint32_t rights,
                            uint32_t *at)
{
    if (status != KERR_OK) {
        s->what = "carve what a child builds from";
    } else {
        status = child_give(s, c, carved, rights, at);
    }
    if (status != KERR_OK) {
        slot_back(s, carved);
        return status;
    }
    return slot_free(s, carved);
}

uint32_t child_give_own(struct self *s, struct child *c, uint32_t pool_size, uint32_t lines, uint32_t units,
                        uint32_t slots, struct child_own *o)
{
    uint32_t carved;
    if (slots > c->table_slots - c->slots_given || lines > s->timer_line_count) {
        s->what = "slots and timer lines enough for a child's own";
        return KERR_LIMIT;
    }
    /* Its slots first, so that what is given below goes into the slots before them. */
    o->slot_end = c->table_slots;
    o->slot_first = c->table_slots - slots;
    c->table_slots = o->slot_first;
    PASS(child_give(s, c, c->table, RIGHT_W, &o->table));
    PASS(mem_take(s, pool_size, &c->own));
    PASS(mem_make(s, &c->own, CAP_POOL));
    PASS(child_give(s, c, c->own.made, RIGHT_W, &o->pool));
    PASS(slot_free(s, c->own.made));
    c->own.made = 0;
    PASS(slot_new(s, &carved));
    PASS(give_carved(s, c, carved, rv_irq_carve(s->timer_lines, s->timer_line_count - lines, lines, carved),
                     RIGHT_W, &o->timer_lines));
    o->timer_line_count = lines;
    PASS(units_take(s, units, &c->own_unit));
    c->own_units = units;
    PASS(slot_new(s, &carved));
    PASS(give_carved(s, c, carved, rv_time_carve(s->time, c->own_unit, units, carved), RIGHT_W | RIGHT_X, &o->time));
    o->unit_count = units;
    PASS(child_give_bits(s, c, s->inbox, c->bit_fault, &o->watch));
    return KERR_OK;
}

uint32_t child_map(struct self *s, struct child *c, uint32_t frame, uint32_t rights, uint32_t *region)
{
    PASS(take_lowest(s, &c->regions, PROCESS_REGION_SLOTS, region, "a free region of a child's"));
    uint32_t status = rv_process_install(c->process, *region, frame, rights);
    if (status != KERR_OK) {
        c->regions &= ~(1u << *region);
        s->what = "install a frame in a child";
    }
    return status;
}

uint32_t child_unmap(struct self *s, struct child *c, uint32_t region)
{
    if (region <= CHILD_REGION_DATA || region >= PROCESS_REGION_SLOTS || !(c->regions & (1u << region))) {
        s->what = "a region of a child's the parent handed out";
        return KERR_INVALID_ARG;
    }
    TRY(s, "uninstall a frame from a child", rv_process_uninstall(c->process, region));
    c->regions &= ~(1u << region);
    return KERR_OK;
}

uint32_t child_region(struct self *s, struct child *c, uint32_t *region)
{
    return take_lowest(s, &c->regions, PROCESS_REGION_SLOTS, region, "a free region of a child's");
}

uint32_t child_bit(struct self *s, struct child *c, uint32_t *bit)
{
    uint32_t i;
    PASS(take_lowest(s, &c->bits, 32, &i, "a free bit of a child's inbox"));
    *bit = 1u << i;
    return KERR_OK;
}

uint32_t child_start(struct self *s, struct child *c, void (*entry)(struct child_page *), uint32_t units)
{
    uint32_t time;
    TRY(s, "watch a child", rv_thread_watch(c->thread, s->inbox, c->bit_fault));
    TRY(s, "configure a child",
        rv_thread_configure(c->thread, (uint32_t)(uintptr_t)entry, c->data.base + c->data.size,
                            (uint32_t)(uintptr_t)c->page));
    /*
     * Its units, carved for the bind alone: the binding stays with the thread once the slot is deleted;
     * or none, bound through the parent's own capability, whose RIGHT_X lets it run on spare time.
     */
    if (units == 0) {
        TRY(s, "bind a child to no units", rv_time_bind(s->time, c->thread, 0, 0));
    } else {
        PASS(units_take(s, units, &c->unit));
        c->units = units;
        PASS(slot_new(s, &time));
        uint32_t status = rv_time_carve(s->time, c->unit, units, time);
        if (status == KERR_OK) {
            status = rv_time_bind(time, c->thread, 0, units);
        }
        PASS(slot_free(s, time));
        TRY(s, "bind a child to its units", status);
    }
    TRY(s, "start a child", rv_thread_resume(c->thread));
    return KERR_OK;
}

uint32_t child_tell(const struct child *c)
{
    return rv_signal(c->inbox, CHILD_BIT_PARENT);
}

int child_poll(struct child *c, uint32_t *state)
{
    *state = c->page->state;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (*state == c->told) {
        return 0;
    }
    c->told = *state;
    return 1;
}

int child_check(struct child *c)
{
    int answered = c->page->answered == c->page->asked;
    c->page->asked++;
    child_tell(c);
    return answered;
}

/* The first failure of steps that all run whatever came before, and its step. */
struct first {
    uint32_t status;
    const char *what;
};

static void note(struct self *s, struct first *f, const char *step, uint32_t status)
{
    if (status != KERR_OK && f->status == KERR_OK) {
        f->status = status;
        f->what = step != 0 ? step : s->what;
    }
}

uint32_t child_free(struct self *s, struct child *c)
{
    struct first f = { KERR_OK, 0 };
    /*
     * Its pool: its objects go with it, and so does every capability to them, wherever it lies;
     * its thread's binding to its units goes with the thread, and its timer line with the Irq.
     */
    if (c->own.untyped != 0) {
        note(s, &f, 0, mem_give(s, &c->own));
    }
    if (c->pool.untyped != 0) {
        note(s, &f, 0, mem_give(s, &c->pool));
    }
    units_give(s, c->unit, c->units);
    units_give(s, c->own_unit, c->own_units);
    const uint32_t slots[] = { c->table, c->process, c->thread, c->inbox };
    for (uint32_t i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
        if (slots[i] != 0) {
            note(s, &f, 0, slot_free(s, slots[i]));
        }
    }
    /*
     * Its data: the revoke uninstalls it from the parent's region too, which is marked free here;
     * or, carved from the room, it went from the child with its process, and the room's region stays.
     */
    if (c->page != 0 && !c->in_room) {
        s->regions &= ~(1u << c->region);
    }
    if (c->data.untyped != 0) {
        note(s, &f, 0, mem_give(s, &c->data));
    } else if (c->in_room && c->data.size != 0) {
        if (c->data.made != 0) {
            note(s, &f, 0, slot_free(s, c->data.made));
        }
        room_give(s, &c->data);
    }
    bit_free(s, c->bit_page);
    bit_free(s, c->bit_fault);
    memset(c, 0, sizeof(*c));
    if (f.status != KERR_OK) {
        s->what = f.what;
    }
    return f.status;
}

void child_report(struct child_page *p, uint32_t state)
{
    __atomic_thread_fence(__ATOMIC_RELEASE);
    p->state = state;
    rv_signal(CHILD_PARENT, NOTIFY_ALL_BITS);
}

void child_stop(struct child_page *p, uint32_t state)
{
    child_report(p, state);
    for (;;) {
        uint32_t bits;
        rv_wait(CHILD_INBOX, &bits);
    }
}

void child_fail(struct child_page *p, uint32_t step, uint32_t detail)
{
    p->step = step;
    p->detail = detail;
    child_stop(p, CHILD_FAILED);
}

void child_self(struct self *s, const struct child_own *o)
{
    memset(s, 0, sizeof(*s));
    s->table = o->table;
    s->process = CHILD_SELF;
    s->pool = o->pool;
    s->inbox = CHILD_INBOX;
    s->timer_lines = o->timer_lines;
    s->timer_line_count = o->timer_line_count;
    s->time = o->time;
    s->debug = CHILD_LOG;
    s->slot_first = o->slot_first;
    s->slot_end = o->slot_end;
    s->regions = ~0u;
    s->bits = ~0u;
    for (uint32_t i = o->unit_count; i < TIME_UNITS; i++) {
        s->units[i / 32] |= 1u << (i % 32);
    }
}

uint32_t child_sleep(uint32_t us)
{
    uint32_t bits = 0, other = 0;
    rv_timer_set(CHILD_TIMER, CHILD_BIT_TIMER, us);
    while (!(bits & CHILD_BIT_TIMER)) {
        if (rv_wait(CHILD_INBOX, &bits) != KERR_OK) {
            rv_breakpoint();
        }
        other |= bits;
    }
    return other & ~CHILD_BIT_TIMER;
}
