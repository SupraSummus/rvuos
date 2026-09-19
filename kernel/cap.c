/*
 * Capability tables.
 */

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

int cap_store(struct captable *table, uint32_t slot, const struct cap *cap)
{
    int err = cap_slot_free(table, slot);
    if (err == KERR_OK) {
        table->slots[slot] = *cap;
    }
    return err;
}

int cap_clear(struct captable *table, uint32_t slot)
{
    if (slot >= table->nslots) {
        return KERR_INVALID_CAP;
    }
    table->slots[slot].type = CAP_NONE;
    return KERR_OK;
}

void cap_revoke_range(uint32_t base, uint32_t size)
{
    for (struct obj_header *o = object_first(); o != NULL; o = object_next(o)) {
        if (o->type != CAP_CAPTABLE) {
            continue;
        }
        struct captable *table = (struct captable *)o;
        for (uint32_t i = 0; i < table->nslots; i++) {
            struct cap *c = &table->slots[i];
            if (cap_has_object(c->type) && range_contains(base, size, c->a)) {
                c->type = CAP_NONE;
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
