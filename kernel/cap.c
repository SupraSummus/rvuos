/*
 * Capability tables and the derivation tree over their slots.
 * See DESIGN.md, "The derivation tree".
 */

#include "kernel.h"
#include "object.h"
#include "sched.h"
#include "trap.h"

int cap_lookup(struct captable *table, uint32_t slot, struct cap *out)
{
    if (slot >= table->nslots) {
        return KERR_INVALID_CAP;
    }
    struct cap c = table->slots[slot];
    if (c.type == CAP_NONE) {
        return KERR_INVALID_CAP;
    }
    /*
     * A destroy clears the slots naming what it takes, so this never fires.
     * It costs one load and stops a hole in that sweep
     * from becoming a cast of user memory to a kernel object.
     */
    if (cap_has_object(c.type) && cap_object(&c)->type != c.type) {
        return KERR_INVALID_CAP;
    }
    *out = c;
    return KERR_OK;
}

int cap_lookup_typed(struct captable *table, uint32_t slot,
                     uint8_t type, uint8_t rights, struct cap *out)
{
    int err = cap_lookup(table, slot, out);
    if (err != KERR_OK) {
        return err;
    }
    if (out->type != type) {
        return KERR_WRONG_TYPE;
    }
    if ((out->rights & rights) != rights) {
        return KERR_NO_RIGHTS;
    }
    return KERR_OK;
}

int cap_slot_free(const struct captable *table, uint32_t slot)
{
    if (slot >= table->nslots) {
        return KERR_INVALID_CAP;
    }
    if (table->slots[slot].type != CAP_NONE) {
        return KERR_SLOT_IN_USE;
    }
    return KERR_OK;
}

/* The node a link names, which is not 0. */
static inline struct cap *node(uint32_t link)
{
    return link_node(link);
}

/* True if [base, base + size) lies within [in, in + insize). */
static bool range_within(uint32_t base, uint32_t size, uint32_t in, uint32_t insize)
{
    return base >= in && base - in <= insize - size && size <= insize;
}

/* A node naming an object in a pool: a capability to one, or a thread's or an Irq's hold on one. */
static bool names_object(uint8_t type)
{
    return type == CAP_POOL || type == CAP_CAPTABLE || type == CAP_PROCESS || type == CAP_THREAD ||
           type == CAP_NOTIFICATION || type == CAP_IRQ || type == CAP_HOSTED || type == CAP_WATCHED ||
           type == CAP_SIGNALLED;
}

/* The memory a node names, if it names memory: its block, its range or its pool's, or the first byte of its object. */
static bool node_memory(const struct cap *n, uint32_t *base, uint32_t *size)
{
    switch (n->type) {
    case CAP_UNTYPED:
        *base = untyped_base(n);
        *size = untyped_size(n);
        return true;
    case CAP_FRAME:
    case CAP_INSTALLED:
        *base = n->a;
        *size = n->b;
        return true;
    case CAP_RETYPED:
        *base = n->a;
        *size = ((const struct pool *)p2v(n->a))->size;
        return true;
    default:
        *base = n->a;
        *size = 1;
        return names_object(n->type);
    }
}

bool node_below(const struct cap *above, const struct cap *n)
{
    if (!above->orig) {
        return false;
    }
    uint32_t base, size, abase, asize;
    switch (above->type) {
    case CAP_UNTYPED:
        /* What it made, a half, a frame or a pool, and what lies below those, all of it in its memory. */
        node_memory(above, &abase, &asize);
        return node_memory(n, &base, &size) && range_within(base, size, abase, asize);
    case CAP_RETYPED:
    case CAP_POOL:
        /* The pool and its objects, and what names them. */
        abase = above->a;
        asize = ((const struct pool *)p2v(above->a))->size;
        return names_object(n->type) && range_contains(abase, asize, n->a);
    case CAP_FRAME:
        return (n->type == CAP_FRAME || n->type == CAP_INSTALLED) && range_within(n->a, n->b, above->a, above->b);
    case CAP_IRQ_LINE:
        return n->type == CAP_IRQ_LINE && range_within(n->a, n->b, above->a, above->b);
    case CAP_TIME:
        /* A thread's units lie among those it was bound through, and the first of them does even for none. */
        return (n->type == CAP_TIME || n->type == CAP_BOUND) && n->a - above->a < above->b &&
               n->b <= above->b - (n->a - above->a);
    case CAP_CLOCK: {
        /* The counter's block, which a clock gives out, and what is carved or installed from it. */
        const struct granted_range *g = &boot_granted[GRANT_COUNTER];
        return n->type == CAP_CLOCK ||
               ((n->type == CAP_FRAME || n->type == CAP_INSTALLED) && range_within(n->a, n->b, g->base, g->size));
    }
    case CAP_PRINT:
    case CAP_HALT:
    case CAP_TEST:
        return n->type == above->type;
    default:
        /* A capability to an object: what names the same object. */
        return names_object(n->type) && n->a == above->a;
    }
}

void cap_attach(struct cap *after, struct cap *n)
{
    n->prev = 0;
    n->next = 0;
    if (after == NULL) {
        return;
    }
    n->prev = node_link(after);
    n->next = after->next;
    if (after->next != 0) {
        node(after->next)->prev = node_link(n);
    }
    after->next = node_link(n);
}

/* Take a node out of its list; what lay below it stays where it was, below what lay above it. */
static void unlink_node(struct cap *n)
{
    if (n->prev != 0) {
        node(n->prev)->next = n->next;
    }
    if (n->next != 0) {
        node(n->next)->prev = n->prev;
    }
}

void cap_move(struct cap *from, struct cap *to)
{
    /* Two links name a node from outside it: its predecessor's next and its successor's previous. */
    *to = *from;
    if (to->prev != 0) {
        node(to->prev)->next = node_link(to);
    }
    if (to->next != 0) {
        node(to->next)->prev = node_link(to);
    }
    *from = (struct cap){ 0 };
}

int cap_store(struct captable *table, uint32_t slot, const struct cap *cap, struct cap *after)
{
    int err = cap_slot_free(table, slot);
    if (err == KERR_OK) {
        table->slots[slot] = *cap;
        cap_attach(after, &table->slots[slot]);
    }
    return err;
}

/*
 * Empty a node the tree no longer names.
 * An installed region is unmapped as well, a thread's units unbound, a thread whose process goes stopped,
 * and an Irq whose notification goes disarmed.
 */
static void clear_node(struct cap *n)
{
    if (n->type == CAP_INSTALLED) {
        process_drop(n);
    } else if (n->type == CAP_BOUND) {
        sched_unbind(n);
    } else if (n->type == CAP_HOSTED) {
        sched_unhost(n);
    } else if (n->type == CAP_SIGNALLED) {
        irq_drop(n);
    } else {
        *n = (struct cap){ 0 };
    }
}

uint32_t preempt_countdown;

bool cap_stop_here(bool preempt)
{
    if (!preempt) {
        return false;
    }
    /* Every place counts, pending or not, so the host, where one always is, stops where QEMU does. */
    bool pending = intr_pending();
    if (preempt_countdown != 0 && --preempt_countdown == 0) {
        core_self()->preempt_stopped = true;
        return true;
    }
    return pending;
}

bool cap_revoke_step(struct cap *root)
{
    struct cap *n = node_first_below(root);
    if (n == NULL) {
        return false;
    }
    unlink_node(n);
    clear_node(n);
    return true;
}

bool cap_revoke_below(struct cap *root, uint32_t through, bool preempt)
{
    /*
     * The node right after the root goes while it lies below the root, and what lay below it stays there,
     * so every step clears one node in constant time,
     * the tree is whole between any two steps,
     * and the next node to clear is always the one right after the root:
     * a walk that stops keeps its progress in the tree and nowhere else.
     * A pool's own node is the one exception: the pool goes before its node does,
     * and the node stays right after the root until then, so the next step is at it again.
     */
    struct cap *n;
    while ((n = node_first_below(root)) != NULL) {
        LOOP_PAID(cap_revoke_below, node, "a node it clears");
        if (n->type == CAP_RETYPED) {
            if (!pool_destroy(node_pool(n), preempt)) {
                return false;
            }
        } else {
            unlink_node(n);
            clear_node(n);
        }
        /*
         * A revoke that took the process its caller runs in, the table the call's slot lies in,
         * or the capability it was made through ends there:
         * the caller could not make the call again, so an interrupt must not decide how far it got.
         * A table slot of its process can only be cleared here, never filled, so the number still names the same slot.
         */
        const struct cap *at = slot_at(core_self()->current, through);
        if (at == NULL || at->type == CAP_NONE) {
            return true;
        }
        if (node_first_below(root) != NULL && cap_stop_here(preempt)) {
            return false;
        }
    }
    return true;
}

void cap_delete(struct cap *n)
{
    /* What lay below it lies below what lay above it, or is a root where nothing did: no walk, roots or not. */
    unlink_node(n);
    clear_node(n);
}

int cap_clear(struct captable *table, uint32_t slot)
{
    if (slot >= table->nslots) {
        return KERR_INVALID_CAP;
    }
    if (table->slots[slot].type != CAP_NONE) {
        cap_delete(&table->slots[slot]);
    }
    return KERR_OK;
}

struct cap cap_to_object(struct obj_header *obj, uint8_t rights)
{
    struct cap c = {
        .type = obj->type,
        .rights = rights,
        .a = v2p(obj),
        .b = obj->type == CAP_NOTIFICATION ? NOTIFY_ALL_BITS : 0,
    };
    return c;
}

struct cap cap_to_frame(uint32_t base, uint32_t size, uint8_t rights)
{
    struct cap c = {
        .type = CAP_FRAME,
        .rights = rights,
        .a = base,
        .b = size,
    };
    return c;
}

struct cap cap_to_untyped(uint32_t base, uint32_t size, uint8_t rights)
{
    struct cap c = {
        .type = CAP_UNTYPED,
        .rights = rights,
        .a = untyped_block(base, size),
        .b = 0,
    };
    return c;
}

struct cap cap_to_lines(uint32_t first, uint32_t count, uint8_t rights)
{
    struct cap c = {
        .type = CAP_IRQ_LINE,
        .rights = rights,
        .a = first,
        .b = count,
    };
    return c;
}

struct cap cap_to_time(uint32_t first, uint32_t count, uint8_t rights)
{
    struct cap c = {
        .type = CAP_TIME,
        .rights = rights,
        .a = first,
        .b = count,
    };
    return c;
}
