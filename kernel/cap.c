/*
 * Capability tables and the derivation tree over their slots.
 * See DESIGN.md, "The derivation tree".
 */

#include "kernel.h"
#include "object.h"
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
    return (n->next & LINK_UP) ? node(node(n->next)->child) : node(n->next);
}

void cap_attach(struct cap *parent, struct cap *n)
{
    n->child = 0;
    if (parent == NULL) {
        n->next = LINK_UP;
        n->prev = 0;
        return;
    }
    /* At the front of the ring; the last child keeps the link up to the parent. */
    if (parent->child == 0) {
        n->next = v2p(parent) | LINK_UP;
        n->prev = v2p(n);
    } else {
        struct cap *first = node(parent->child);
        n->next = parent->child;
        n->prev = first->prev;
        first->prev = v2p(n);
    }
    parent->child = v2p(n);
}

/* Put the siblings first to last right after beside, which has a parent, in its ring. */
static void splice_after(struct cap *beside, struct cap *first, struct cap *last)
{
    ring_next(beside)->prev = v2p(last);
    last->next = beside->next;
    first->prev = v2p(beside);
    beside->next = v2p(first);
}

/* Right after beside in its ring, so under the same parent. Roots have no ring: beside a root, a root. */
static void attach_beside(struct cap *beside, struct cap *n)
{
    n->child = 0;
    if (beside->next == LINK_UP) {
        n->next = LINK_UP;
        n->prev = 0;
        return;
    }
    splice_after(beside, n, n);
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

/* Empty a node the tree no longer names. An installed region is unmapped as well. */
static void clear_node(struct cap *n)
{
    if (n->type == CAP_INSTALLED) {
        process_drop(n);
    } else {
        *n = (struct cap){ 0 };
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
    if (adopt && n->child != 0) {
        splice_after(n, node(n->child), node(node(n->child)->prev));
    }
    struct cap *pred = node(n->prev);
    ring_next(n)->prev = n->prev;
    /* n is first exactly when its predecessor links up: it is the last child then, or n itself. */
    if (pred->next & LINK_UP) {
        node(pred->next)->child = (n->next & LINK_UP) ? 0 : n->next;
    } else {
        pred->next = n->next;
    }
}

bool cap_stop_here(bool preempt)
{
    return preempt && intr_pending();
}

bool cap_revoke_step_except(struct cap *root, const struct cap *keep)
{
    if (root->child == 0) {
        return false;
    }
    struct cap *n = node(root->child);
    if (n == keep) {
        /* keep stays first, so the next step looks past it again. */
        if (keep->next & LINK_UP) {
            return false;
        }
        n = node(keep->next);
    }
    detach(n, true);
    clear_node(n);
    return true;
}

bool cap_revoke_below(struct cap *root, bool preempt)
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
    while (root->child != 0) {
        LOOP_PAID(cap_revoke_below, node, "a node it clears");
        struct cap *n = node(root->child);
        if (n->type == CAP_RETYPED) {
            if (!pool_destroy(node_pool(n), NULL, preempt)) {
                return false;
            }
        } else {
            detach(n, true);
            clear_node(n);
        }
        if (root->child != 0 && cap_stop_here(preempt)) {
            return false;
        }
    }
    return true;
}

bool cap_delete(struct cap *n, bool preempt)
{
    if (n->next != LINK_UP) {
        detach(n, true);
        clear_node(n);
        return true;
    }
    /* Below a root there is no ring to hand the children to: each becomes a root, one step at a time. */
    while (n->child != 0) {
        LOOP_PAID(cap_delete, link, "a child below a root, which becomes a root itself");
        struct cap *c = node(n->child);
        detach(c, false);
        c->next = LINK_UP;
        c->prev = 0;
        if (n->child != 0 && cap_stop_here(preempt)) {
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
    struct cap c = {
        .type = obj->type,
        .rights = rights,
        .a = v2p(obj),
        .b = 0,
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
