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
    if (c.type != CAP_REGION && c.type != CAP_DEBUG) {
        struct obj_header *obj = cap_object(&c);
        if (obj->type != c.type || obj->generation != c.generation) {
            /* The object died; drop the stale slot while we are here. */
            table->slots[slot].type = CAP_NONE;
            return KERR_INVALID_CAP;
        }
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

struct cap cap_to_object(struct obj_header *obj, uint8_t rights)
{
    struct cap c = {
        .type = obj->type,
        .rights = rights,
        .generation = obj->generation,
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
        .generation = 0,
        .a = base,
        .b = size,
    };
    return c;
}
