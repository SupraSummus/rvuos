/*
 * Capability tables and the derivation tree over their slots.
 * See DESIGN.md, "The derivation tree".
 */

#include "dma.h"
#include "kernel.h"
#include "object.h"
#include "route.h"
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

/* The node a link names, the tag stripped. */
static inline struct cap *node(uint32_t link)
{
    return p2v(link & ~LINK_UP);
}

/* The sibling after a node that has a parent, the first one after the last. */
static struct cap *ring_next(const struct cap *n)
{
    return (cap_next(n) & LINK_UP) ? node(cap_child(node(cap_next(n)))) : node(cap_next(n));
}

/* A node new to the tree has no child yet; a held one names its holder there instead, and never has one. */
static void make_childless(struct cap *n)
{
    if (!cap_held(n)) {
        cap_set_child(n, 0);
    }
}

void cap_attach(struct cap *parent, struct cap *n)
{
    make_childless(n);
    if (parent == NULL) {
        cap_set_next(n, LINK_UP);
        cap_set_prev(n, 0);
        return;
    }
    /* At the front of the ring; the last child keeps the link up to the parent. */
    if (cap_child(parent) == 0) {
        cap_set_next(n, v2p(parent) | LINK_UP);
        cap_set_prev(n, v2p(n));
    } else {
        struct cap *first = node(cap_child(parent));
        cap_set_next(n, cap_child(parent));
        cap_set_prev(n, cap_prev(first));
        cap_set_prev(first, v2p(n));
    }
    cap_set_child(parent, v2p(n));
}

/* Put the siblings first to last right after beside, which has a parent, in its ring. */
static void splice_after(struct cap *beside, struct cap *first, struct cap *last)
{
    cap_set_prev(ring_next(beside), v2p(last));
    cap_set_next(last, cap_next(beside));
    cap_set_prev(first, v2p(beside));
    cap_set_next(beside, v2p(first));
}

/* Right after beside in its ring, so under the same parent. Roots have no ring: beside a root, a root. */
static void attach_beside(struct cap *beside, struct cap *n)
{
    make_childless(n);
    if (cap_next(beside) == LINK_UP) {
        cap_set_next(n, LINK_UP);
        cap_set_prev(n, 0);
        return;
    }
    splice_after(beside, n, n);
}

void cap_move(struct cap *from, struct cap *to)
{
    /*
     * Three links at most name a node from outside it:
     * its predecessor's next, or its parent's child when it is first,
     * the previous link of the sibling after it, the first one when it is last,
     * and the link up from its last child.
     * An only child is its own predecessor and successor, and a root is in no ring.
     */
    *to = *from;
    if (cap_next(from) != LINK_UP) {
        struct cap *pred = node(cap_prev(from));
        if (pred == from) {
            cap_set_prev(to, v2p(to));
        } else {
            cap_set_prev(ring_next(from), v2p(to));
        }
        if (cap_next(pred) & LINK_UP) {
            cap_set_child(node(cap_next(pred)), v2p(to));
        } else {
            cap_set_next(pred, v2p(to));
        }
    }
    if (cap_child(from) != 0) {
        cap_set_next(node(cap_prev(node(cap_child(from)))), v2p(to) | LINK_UP);
    }
    *from = (struct cap){ 0 };
}

int cap_store(struct captable *table, uint32_t slot, const struct cap *cap, struct cap *parent)
{
    int err = cap_slot_free(table, slot);
    if (err == KERR_OK) {
        table->slots[slot] = *cap;
        cap_attach(parent, &table->slots[slot]);
    }
    return err;
}

int cap_store_beside(struct captable *table, uint32_t slot, const struct cap *cap, struct cap *beside)
{
    int err = cap_slot_free(table, slot);
    if (err == KERR_OK) {
        table->slots[slot] = *cap;
        attach_beside(beside, &table->slots[slot]);
    }
    return err;
}

/*
 * Empty a node the tree no longer names.
 * An installed region is unmapped as well, a thread's units unbound, a thread whose process goes stopped,
 * an Irq whose notification goes disarmed, a Route whose end goes too, and a Dma whose end goes halted:
 * the nodes whose holder hears of their clearing.
 */
static void clear_node(struct cap *n)
{
    if (!cap_held(n)) {
        *n = (struct cap){ 0 };
    } else if (n->type == CAP_FRAME) {
        /* A region of a process, or an end of a Route or a Dma: the holder says which. */
        uint8_t holder = ((struct obj_header *)p2v(cap_holder(n)))->type;
        if (holder == CAP_ROUTE) {
            route_drop(n);
        } else if (holder == CAP_DMA) {
            dma_drop(n);
        } else {
            process_drop(n);
        }
    } else if (n->type == CAP_TIME) {
        sched_unbind(n);
    } else if (n->type == CAP_PROCESS) {
        sched_unhost(n);
    } else {
        irq_drop(n);
    }
}

/*
 * Take a node that has a parent out of its parent's ring.
 * With adopt, what was derived from it takes its place in the ring,
 * so its parent's ring keeps the whole of the derivation below;
 * without, the node's children stay below it.
 */
static void detach(struct cap *n, bool adopt)
{
    /* The children go right after n, so n leaves the ring as a leaf would. */
    if (adopt && cap_child(n) != 0) {
        splice_after(n, node(cap_child(n)), node(cap_prev(node(cap_child(n)))));
    }
    struct cap *pred = node(cap_prev(n));
    cap_set_prev(ring_next(n), cap_prev(n));
    /* n is first exactly when its predecessor links up: it is the last child then, or n itself. */
    if (cap_next(pred) & LINK_UP) {
        cap_set_child(node(cap_next(pred)), (cap_next(n) & LINK_UP) ? 0 : cap_next(n));
    } else {
        cap_set_next(pred, cap_next(n));
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
    return pending || core_notice_owed();
}

bool cap_revoke_step(struct cap *root)
{
    if (cap_child(root) == 0) {
        return false;
    }
    struct cap *n = node(cap_child(root));
    detach(n, true);
    clear_node(n);
    return true;
}

bool cap_revoke_below(struct cap *root, uint32_t through, bool preempt)
{
    /*
     * The first child goes and its children take its place at the front of the ring,
     * so every step clears one node in constant time,
     * the tree is whole between any two steps,
     * and the next node to clear is always the root's first child:
     * a walk that stops keeps its progress in the tree and nowhere else.
     * A pool's own node is the one exception: the pool goes before its node does,
     * and the node stays first until then, so the next step is at it again.
     */
    while (cap_child(root) != 0) {
        LOOP_PAID(cap_revoke_below, node, "a node it clears");
        struct cap *n = node(cap_child(root));
        struct pool *pool = node_pool(n);
        if (pool != NULL) {
            if (!pool_destroy(pool, preempt)) {
                return false;
            }
        } else {
            detach(n, true);
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
        if (cap_child(root) != 0 && cap_stop_here(preempt)) {
            return false;
        }
    }
    return true;
}

bool cap_delete(struct cap *n, bool preempt)
{
    if (cap_next(n) != LINK_UP) {
        detach(n, true);
        clear_node(n);
        return true;
    }
    /*
     * Below a root there is no ring to hand the children to: each becomes a root, one step at a time.
     * An Untyped makes nothing more, as those already roots lie in its memory
     * while another thread may take away those still below it.
     */
    if (n->type == CAP_UNTYPED) {
        untyped_set_going(n);
    }
    while (cap_child(n) != 0) {
        LOOP_PAID(cap_delete, link, "a child below a root, which becomes a root itself");
        struct cap *c = node(cap_child(n));
        detach(c, false);
        cap_set_next(c, LINK_UP);
        cap_set_prev(c, 0);
        if (cap_child(n) != 0 && cap_stop_here(preempt)) {
            return false;
        }
    }
    clear_node(n);
    return true;
}

int cap_clear(struct captable *table, uint32_t slot)
{
    if (slot >= table->nslots) {
        return KERR_INVALID_CAP;
    }
    if (table->slots[slot].type != CAP_NONE && !cap_delete(&table->slots[slot], true)) {
        return KERR_PREEMPTED;
    }
    return KERR_OK;
}

struct cap cap_to_object(struct obj_header *obj, uint8_t rights)
{
    struct cap c = { .type = obj->type, .rights = rights, .a = v2p(obj) };
    if (obj->type == CAP_NOTIFICATION) {
        notify_set(&c, v2p(obj), NOTIFY_CODE_ALL);
    }
    return c;
}

struct cap cap_to_frame(uint32_t base, uint32_t size, uint8_t rights)
{
    struct cap c = {
        .type = CAP_FRAME,
        .rights = rights,
        .a = block_word(base, size),
    };
    return c;
}

struct cap cap_to_untyped(uint32_t base, uint32_t size, uint8_t rights)
{
    struct cap c = {
        .type = CAP_UNTYPED,
        .rights = rights,
        .a = block_word(base, size),
    };
    return c;
}

struct cap cap_to_lines(uint32_t first, uint32_t count, uint8_t rights)
{
    struct cap c = {
        .type = CAP_IRQ_LINE,
        .rights = rights,
        .a = range_word(first, count),
    };
    return c;
}

struct cap cap_to_time(uint32_t first, uint32_t count, uint8_t rights)
{
    struct cap c = {
        .type = CAP_TIME,
        .rights = rights,
        .a = range_word(first, count),
    };
    return c;
}

struct cap cap_to_routes(uint32_t first, uint32_t count, uint8_t rights)
{
    struct cap c = {
        .type = CAP_ROUTE_LINE,
        .rights = rights,
        .a = range_word(first, count),
    };
    return c;
}

struct cap cap_to_dma(uint32_t first, uint32_t count, uint8_t rights)
{
    struct cap c = {
        .type = CAP_DMA_LINE,
        .rights = rights,
        .a = range_word(first, count),
    };
    return c;
}
