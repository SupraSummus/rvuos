/*
 * Capability tables and the derivation tree over their slots.
 * See DESIGN.md, "The derivation tree".
 */

#include "kernel.h"
#include "object.h"

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

void cap_attach(struct cap *parent, struct cap *n)
{
    n->child = 0;
    if (parent == NULL) {
        n->next = LINK_UP;
        return;
    }
    /* At the front of the ring; the last child keeps the link up to the parent. */
    n->next = parent->child != 0 ? parent->child : (v2p(parent) | LINK_UP);
    parent->child = v2p(n);
}

/* Right after beside in its ring, so under the same parent. Roots have no ring: beside a root, a root. */
static void attach_beside(struct cap *beside, struct cap *n)
{
    n->child = 0;
    if (beside->next == LINK_UP) {
        n->next = LINK_UP;
        return;
    }
    n->next = beside->next;
    beside->next = v2p(n);
}

struct cap *cap_parent(const struct cap *n)
{
    uint32_t link = n->next;
    while (!(link & LINK_UP)) {
        link = node(link)->next;
    }
    return link == LINK_UP ? NULL : node(link);
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
 * Take a node out of its parent's ring.
 * With adopt, what was derived from it takes its place in the ring,
 * so its parent's ring keeps the whole of the derivation below;
 * without, the node's children are gone already.
 * Below a root there is no ring: adopted children become roots themselves.
 */
static void detach(struct cap *n, bool adopt)
{
    uint32_t first = adopt ? n->child : 0;

    if (n->next == LINK_UP) {
        for (uint32_t link = first; link != 0 && !(link & LINK_UP);) {
            struct cap *c = node(link);
            link = c->next;
            c->next = LINK_UP;
        }
        return;
    }

    struct cap *parent = cap_parent(n);
    /* What follows n's predecessor once n is gone: n's children, or n's successor. */
    uint32_t replacement = n->next;
    if (first != 0) {
        struct cap *last = node(first);
        while (!(last->next & LINK_UP)) {
            last = node(last->next);
        }
        last->next = n->next;
        replacement = first;
    }
    if (node(parent->child) == n) {
        parent->child = (replacement & LINK_UP) ? 0 : replacement;
    } else {
        struct cap *pred = node(parent->child);
        while (node(pred->next) != n) {
            pred = node(pred->next);
        }
        pred->next = replacement;
    }
}

void cap_revoke_below(struct cap *root)
{
    /*
     * Post-order, with no stack: descend to a leaf, clear it, move to its next;
     * a link up says every child of that parent is cleared,
     * so the parent goes too, and the walk continues with the parent's next.
     * The root's own ring closes at the root, which is where the walk stops.
     */
    uint32_t at = root->child;
    while (at != 0) {
        struct cap *n = node(at);
        if (at & LINK_UP) {
            if (n == root) {
                break;
            }
            at = n->next;
            clear_node(n);
            continue;
        }
        if (n->child != 0) {
            at = n->child;
            continue;
        }
        at = n->next;
        clear_node(n);
    }
    root->child = 0;
}

void cap_revoke(struct cap *n)
{
    cap_revoke_below(n);
    detach(n, false);
    clear_node(n);
}

void cap_delete(struct cap *n)
{
    detach(n, true);
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

void cap_revoke_range(uint32_t base, uint32_t size)
{
    for (struct obj_header *o = object_first(); o != NULL; o = object_next(o)) {
        bool dying = range_contains(base, size, v2p(o));
        if (o->type == CAP_CAPTABLE) {
            struct captable *table = (struct captable *)o;
            for (uint32_t i = 0; i < table->nslots; i++) {
                struct cap *c = &table->slots[i];
                if (c->type == CAP_NONE) {
                    continue;
                }
                /* A node the range holds takes what was derived from it: a grant dies with its slot. */
                if (dying || (cap_has_object(c->type) && range_contains(base, size, c->a))) {
                    cap_revoke(c);
                }
            }
        } else if (o->type == CAP_PROCESS && dying) {
            struct process *proc = (struct process *)o;
            for (unsigned i = 0; i < PROCESS_REGION_SLOTS; i++) {
                if (proc->slots[i].type != CAP_NONE) {
                    cap_revoke(&proc->slots[i]);
                }
            }
        }
    }
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

struct cap cap_to_region(uint32_t base, uint32_t size, uint8_t rights)
{
    struct cap c = {
        .type = CAP_REGION,
        .rights = rights,
        .a = base,
        .b = size,
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
