/*
 * The abstraction function: the state of the derivation tree as the model in model/ reads it,
 * written around every trap when the harness runs with --model-trace=FILE.
 *
 * Each trap is one step of the trace:
 * the event, the state before, what the caller's registers hold after, and the state after.
 * The state is every object with its nodes, and a pool its size,
 * every filled node by its address and its words,
 * and every node's children in the order of their ring, first child first.
 * Links, pointers and the kernel's other state are left out:
 * the model says what a step does to the tree, and the checker holds the kernel to it;
 * see DESIGN.md, "The model".
 *
 * A step is written whole at its end, so a halt, which never returns from its call, writes none;
 * an invariant report ends it early, with the state the report found and a line that says so, see host_violated.
 *
 * Where the records of an input begin, the trace says so with the machine, the state and the driver's threads that may run,
 * for the model to search from; see model_start.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "harness.h"

static FILE *model_out;

/* The step as it is written, held until its end. */
static char *text;
static size_t text_len, text_cap;

static const struct thread *model_caller;
static bool model_pending;
static bool model_call;
static bool model_reported;
static uint32_t model_pc;

void model_open(const char *path)
{
    model_out = fopen(path, "w");
    if (model_out == NULL) {
        perror(path);
        exit(1);
    }
}

static void put(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        abort();
    }
    if (text_len + (size_t)n + 1 > text_cap) {
        text_cap = (text_len + (size_t)n + 1) * 2;
        text = realloc(text, text_cap);
        if (text == NULL) {
            abort();
        }
    }
    va_start(ap, fmt);
    vsnprintf(text + text_len, text_cap - text_len, fmt, ap);
    va_end(ap);
    text_len += (size_t)n;
}

/* The node a link names, the tag stripped. */
static const struct cap *node_at(uint32_t link)
{
    return p2v(link & ~LINK_UP);
}

/*
 * The model's own types for the nodes an object holds, which the kernel tells by B_HELD and by where they lie;
 * see capInstalled in model/Rvuos/Abi.lean.
 */
enum {
    MODEL_INSTALLED = 0x80,
    MODEL_RETYPED = 0x81,
    MODEL_BOUND = 0x82,
    MODEL_HOSTED = 0x83,
    MODEL_SIGNALLED = 0x84,
    MODEL_WATCHED = 0x85,
    MODEL_ROUTED = 0x86,
};

/*
 * A filled node as the model reads it: its type, rights, index, and words,
 * a frame's base and size and a range's first and count in two words,
 * a Notification capability's bits in the second, an Untyped's going there,
 * and a node an object holds by the model's type for it, with its index among its holder's slots or places.
 */
static void put_node(const struct obj_header *o, const struct cap *c)
{
    uint32_t type = c->type, index = 0, a = 0, b = 0;
    switch (c->type) {
    case CAP_FRAME:
    case CAP_UNTYPED:
        a = c->type == CAP_UNTYPED ? c->a : block_base(c);
        b = c->type == CAP_UNTYPED ? untyped_going(c) : block_size(c);
        if (cap_held(c) && o->type == CAP_ROUTE) {
            type = MODEL_ROUTED;
            index = (uint32_t)(c - ((const struct route *)o)->ends);
        } else if (cap_held(c)) {
            type = MODEL_INSTALLED;
            index = (uint32_t)(c - ((const struct process *)o)->slots);
        }
        break;
    case CAP_IRQ_LINE:
    case CAP_TIME:
    case CAP_ROUTE_LINE:
        a = range_first(c);
        b = range_count(c);
        if (cap_held(c)) {
            type = MODEL_BOUND;
            index = (uint32_t)(c - ((const struct thread *)o)->time);
        }
        break;
    case CAP_NOTIFICATION:
        a = cap_addr(c);
        b = notify_bits(c);
        if (cap_held(c)) {
            type = MODEL_SIGNALLED;
        } else if (o->type == CAP_THREAD) {
            type = MODEL_WATCHED;
        }
        break;
    case CAP_PRINT:
    case CAP_HALT:
    case CAP_TEST:
    case CAP_CLOCK:
        break;
    default:
        a = cap_addr(c);
        if (c->type == CAP_PROCESS && cap_held(c)) {
            type = MODEL_HOSTED;
        } else if (node_pool(c) != NULL) {
            type = MODEL_RETYPED;
        }
        break;
    }
    put("node %u %u %u %u %u %u\n", (unsigned)v2p(c), (unsigned)type, (unsigned)c->rights, (unsigned)index,
        (unsigned)a, (unsigned)b);
}

static void put_state(void)
{
    put("state\n");
    for (struct obj_header *o = object_first(); o != NULL; o = object_next(o)) {
        uint32_t count;
        const struct cap *nodes = obj_nodes(o, &count);
        put("obj %u %u %u %u %u %u\n", (unsigned)v2p(o), (unsigned)o->type, (unsigned)o->pool,
            nodes != NULL ? (unsigned)v2p(nodes) : 0u, (unsigned)count,
            o->type == CAP_POOL ? (unsigned)((const struct pool *)o)->size : 0u);
    }
    for (struct obj_header *o = object_first(); o != NULL; o = object_next(o)) {
        uint32_t count;
        const struct cap *nodes = obj_nodes(o, &count);
        for (uint32_t i = 0; i < count; i++) {
            const struct cap *c = &nodes[i];
            if (c->type == CAP_NONE) {
                continue;
            }
            put_node(o, c);
            if (cap_child(c) == 0) {
                continue;
            }
            /* The ring from the first child, until the last links up. */
            put("kids %u", (unsigned)v2p(c));
            /* A ring a bug left open ends where there are no more nodes to visit, for the model to refuse. */
            const struct cap *k = node_at(cap_child(c));
            for (uint32_t n = 0; n < HOST_RAM_SIZE / sizeof(struct cap); n++) {
                put(" %u", (unsigned)v2p(k));
                if (cap_next(k) & LINK_UP) {
                    break;
                }
                k = node_at(cap_next(k));
            }
            put("\n");
        }
    }
    put("end\n");
}

/*
 * What the machine fixes: the smallest region, the smallest pool, where a process's tables, a table's slots
 * and a thread's process lie, and the counter's block, its base, size and rights, which a clock gives out.
 */
static void put_machine(void)
{
    const struct granted_range *g = &boot_granted[GRANT_COUNTER];
    put("machine %u %u %u %u %u %u %u %u\n", (unsigned)region_min_size(), (unsigned)POOL_MIN_SIZE,
        (unsigned)offsetof(struct process, tables), (unsigned)offsetof(struct captable, slots),
        (unsigned)offsetof(struct thread, proc), (unsigned)g->base, (unsigned)g->size, (unsigned)g->rights);
}

void model_begin(bool call)
{
    if (model_out == NULL) {
        return;
    }
    text_len = 0;
    model_pending = true;
    model_reported = false;
    model_call = call;
    model_caller = core_self()->current;
    const struct trap_frame *f = &model_caller->frame;
    model_pc = f->pc;
    put("step %s %u\n", call ? "call" : "fault", (unsigned)v2p(model_caller));
    put_machine();
    put("op %u %u %u %u %u %u\n", (unsigned)f->regs[REG_A7], (unsigned)f->regs[REG_A0], (unsigned)f->regs[REG_A1],
        (unsigned)f->regs[REG_A2], (unsigned)f->regs[REG_A3], (unsigned)f->regs[REG_A4]);
    put_state();
}

void model_end(void)
{
    if (model_out == NULL || !model_pending) {
        return;
    }
    model_pending = false;
    if (model_reported) {
        put("report\n");
    }
    /* A call that destroyed its caller's pool leaves no registers to read; holds_caller keeps a revoke from it. */
    if (object_find(v2p(model_caller)) != &model_caller->hdr) {
        put("result gone\n");
    } else if (!model_call) {
        put("result fault\n");
    } else {
        const struct trap_frame *f = &model_caller->frame;
        if (f->pc == model_pc) {
            /* Made again: the registers are as the call found them. */
            put("result again\n");
        } else {
            put("result ret %u %u %u %u %u\n", (unsigned)f->regs[REG_A0], (unsigned)f->regs[REG_A1],
                (unsigned)f->regs[REG_A2], (unsigned)f->regs[REG_A3], (unsigned)f->regs[REG_A4]);
        }
    }
    put_state();
    fwrite(text, 1, text_len, model_out);
    fflush(model_out);
}

void model_report(void)
{
    model_reported = true;
}

/*
 * The records begin: the driver's threads that may run, by actor number, and the state they find.
 * A stopped thread runs only once a record resumes it, so the model does not search from its calls.
 */
void model_start(void)
{
    if (model_out == NULL) {
        return;
    }
    text_len = 0;
    put("start");
    for (unsigned actor = 1; actor <= REPLAY_THREADS; actor++) {
        const struct thread *t = host_actor_thread(actor);
        if (t != NULL && t->state != THREAD_STOPPED) {
            put(" %u %u", actor, (unsigned)v2p(t));
        }
    }
    put("\n");
    put_machine();
    put_state();
    fwrite(text, 1, text_len, model_out);
    fflush(model_out);
}
