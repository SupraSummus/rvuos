#include "lib/child.h"

#include "lib/libc.h"

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

    PASS(mem_frame(s, data_size, &c->data));
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
    TRY(s, "give a child its parent's inbox", rv_cap_copy(c->table, CHILD_PARENT, s->inbox, RIGHT_W));
    TRY(s, "give a child its process", rv_cap_copy(c->table, CHILD_SELF, c->process, RIGHT_W));

    PASS(region_install(s, c->data.made, RIGHT_R | RIGHT_W, &c->region));
    c->page = (struct child_page *)(uintptr_t)c->data.base;
    PASS(slot_free(s, c->data.made));
    c->data.made = 0;
    PASS(bit_new(s, &c->bit_page));
    PASS(bit_new(s, &c->bit_fault));
    /* A child starts from clean memory, whatever the block held before. */
    memset((void *)(uintptr_t)c->data.base, 0, c->data.size);
    c->page->bit = c->bit_page;
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
    /* Its units, carved for the bind alone: the binding stays with the thread once the slot is deleted. */
    PASS(units_take(s, units, &c->unit));
    c->units = units;
    PASS(slot_new(s, &time));
    uint32_t status = rv_time_carve(s->time, c->unit, units, time);
    if (status == KERR_OK) {
        status = rv_time_bind(time, c->thread, 0, units);
    }
    PASS(slot_free(s, time));
    TRY(s, "bind a child to its units", status);
    TRY(s, "start a child", rv_thread_resume(c->thread));
    return KERR_OK;
}

uint32_t child_tell(const struct child *c)
{
    return rv_signal(c->inbox, CHILD_BIT_PARENT);
}

int child_poll(struct child *c, void (*put)(char), uint32_t *state)
{
    c->log_taken = logring_take(&c->page->log, c->log_taken, put);
    *state = c->page->state;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (*state == c->told) {
        return 0;
    }
    c->told = *state;
    return 1;
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
    if (c->pool.untyped != 0) {
        note(s, &f, 0, mem_give(s, &c->pool));
    }
    units_give(s, c->unit, c->units);
    const uint32_t slots[] = { c->table, c->process, c->thread, c->inbox };
    for (uint32_t i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
        if (slots[i] != 0) {
            note(s, &f, 0, slot_free(s, slots[i]));
        }
    }
    /* Its data: the revoke uninstalls it from the parent's region too, which is marked free here. */
    if (c->page != 0) {
        s->regions &= ~(1u << c->region);
    }
    if (c->data.untyped != 0) {
        note(s, &f, 0, mem_give(s, &c->data));
    }
    bit_free(s, c->bit_page);
    bit_free(s, c->bit_fault);
    memset(c, 0, sizeof(*c));
    if (f.status != KERR_OK) {
        s->what = f.what;
    }
    return f.status;
}

void child_put(void *page, char c)
{
    struct child_page *p = page;
    if (logring_put(&p->log, c)) {
        rv_signal(CHILD_PARENT, p->bit);
    }
}

void child_report(struct child_page *p, uint32_t state)
{
    __atomic_thread_fence(__ATOMIC_RELEASE);
    p->state = state;
    rv_signal(CHILD_PARENT, p->bit);
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
