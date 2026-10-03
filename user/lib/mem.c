/*
 * Free memory, as self.h describes it: Untypeds of blocks, halved down to the size asked for.
 */

#include "lib/libc.h"
#include "lib/self.h"

static int ours(const struct self *s, uint32_t slot)
{
    return slot >= s->slot_first && slot < s->slot_end;
}

static int holds(const struct block *b, uint32_t base, uint32_t size)
{
    return b->base <= base && size <= b->size && base - b->base <= b->size - size;
}

/*
 * Halves b, keeping in it the half that holds base and leaving the other in *other.
 * The split's parent is deleted when it is one of the slots handed out, which the halves then hang below the parent of,
 * so a halving costs one slot; one of the boot's stays as it is.
 */
static uint32_t halve(struct self *s, struct block *b, uint32_t base, struct block *other)
{
    uint32_t lower, upper;
    PASS(slot_new(s, &lower));
    uint32_t status = slot_new(s, &upper);
    if (status != KERR_OK) {
        slot_back(s, lower);
        return status;
    }
    status = rv_split(b->untyped, lower, upper);
    if (status != KERR_OK) {
        s->what = "split free memory";
        slot_back(s, lower);
        slot_back(s, upper);
        return status;
    }
    if (ours(s, b->untyped)) {
        PASS(slot_free(s, b->untyped));
    }
    uint32_t half = b->size / 2;
    struct block lo = { lower, 0, b->base, half }, hi = { upper, 0, b->base + half, half };
    *b = base < hi.base ? lo : hi;
    *other = base < hi.base ? hi : lo;
    return KERR_OK;
}

uint32_t mem_take_at(struct self *s, uint32_t base, uint32_t size, struct block *out)
{
    if (size == 0 || (size & (size - 1)) != 0 || (base & (size - 1)) != 0) {
        s->what = "a block's size and place";
        return KERR_INVALID_ARG;
    }
    for (uint32_t i = 0; i < s->free_count; i++) {
        struct block b = s->free[i];
        if (!holds(&b, base, size)) {
            continue;
        }
        /* Each halving leaves a free half, which needs room in the list. */
        uint32_t halvings = 0;
        for (uint32_t at = b.size; at > size; at /= 2) {
            halvings++;
        }
        if (s->free_count - 1 + halvings > SELF_BLOCKS) {
            s->what = "room for free blocks";
            return KERR_LIMIT;
        }
        s->free[i] = s->free[--s->free_count];
        while (b.size > size) {
            struct block other;
            uint32_t status = halve(s, &b, base, &other);
            if (status != KERR_OK) {
                s->free[s->free_count++] = b;
                return status;
            }
            s->free[s->free_count++] = other;
        }
        *out = b;
        return KERR_OK;
    }
    s->what = "free memory holding a block";
    return KERR_NO_MEMORY;
}

uint32_t mem_take(struct self *s, uint32_t size, struct block *b)
{
    const struct block *best = 0;
    for (uint32_t i = 0; i < s->free_count; i++) {
        const struct block *f = &s->free[i];
        if (f->size >= size &&
            (best == 0 || f->size < best->size || (f->size == best->size && f->base < best->base))) {
            best = f;
        }
    }
    if (best == 0) {
        s->what = "free memory";
        return KERR_NO_MEMORY;
    }
    return mem_take_at(s, best->base, size, b);
}

uint32_t mem_make(struct self *s, struct block *b, uint32_t type)
{
    uint32_t made, base;
    PASS(slot_new(s, &made));
    uint32_t status = rv_retype(b->untyped, type, made, &base);
    if (status != KERR_OK) {
        s->what = type == CAP_POOL ? "make a pool" : "make a frame";
        slot_back(s, made);
        return status;
    }
    b->made = made;
    return KERR_OK;
}

uint32_t mem_frame(struct self *s, uint32_t size, struct block *b)
{
    PASS(mem_take(s, size, b));
    uint32_t status = mem_make(s, b, CAP_FRAME);
    if (status != KERR_OK) {
        const char *what = s->what;
        mem_give(s, b);
        s->what = what;
    }
    return status;
}

uint32_t mem_give(struct self *s, struct block *b)
{
    if (s->free_count == SELF_BLOCKS) {
        s->what = "room for a free block";
        return KERR_LIMIT;
    }
    TRY(s, "take a block back", rv_cap_revoke(s->table, b->untyped));
    if (b->made != 0) {
        PASS(slot_free(s, b->made)); /* the revoke emptied it */
    }
    s->free[s->free_count++] = (struct block){ b->untyped, 0, b->base, b->size };
    *b = (struct block){ 0, 0, 0, 0 };
    return KERR_OK;
}

uint32_t mem_read(struct self *s, uint32_t base, void *buf, uint32_t len)
{
    for (uint32_t i = 0; i < s->free_count; i++) {
        struct block b = s->free[i];
        if (!holds(&b, base, len)) {
            continue;
        }
        uint32_t region;
        PASS(mem_make(s, &b, CAP_FRAME));
        uint32_t status = region_install(s, b.made, RIGHT_R, &region);
        if (status == KERR_OK) {
            memcpy(buf, (const void *)(uintptr_t)base, len);
            status = region_free(s, region);
        }
        /* Revoking below the Untyped leaves it free, as it is in the list. */
        const char *what = s->what;
        TRY(s, "give the block read back", rv_cap_revoke(s->table, b.untyped));
        PASS(slot_free(s, b.made));
        s->what = what;
        return status;
    }
    s->what = "free memory holding the bytes";
    return KERR_INVALID_ARG;
}

uint32_t mem_unused(const struct self *s)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < s->free_count; i++) {
        n += s->free[i].size;
    }
    return n;
}
