/*
 * A libFuzzer mutator that works on records rather than bytes.
 *
 * Most fields of a replay_record have small ranges,
 * and a byte flip lands outside them more often than inside,
 * so a handoff between the driver's threads took a lucky guess per byte.
 * Here a mutation inserts, deletes, duplicates, swaps or moves a whole record,
 * or sets one field to a value in its range,
 * and the crossover splices two inputs on record boundaries.
 * A quarter of the time libFuzzer's byte mutations run instead,
 * for the values these ranges leave out.
 *
 * The harness ignores a trailing partial record;
 * the record mutations drop it and the byte mutations keep it.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "rvuos/replay.h"

size_t LLVMFuzzerMutate(uint8_t *data, size_t size, size_t max_size);
size_t LLVMFuzzerCustomMutator(uint8_t *data, size_t size, size_t max_size, unsigned seed);
size_t LLVMFuzzerCustomCrossOver(const uint8_t *data1, size_t size1,
                                 const uint8_t *data2, size_t size2,
                                 uint8_t *out, size_t max_out, unsigned seed);

#define RECORD sizeof(struct replay_record)

/* splitmix32; any seed will do. */
struct rng {
    uint32_t state;
};

static uint32_t rnd(struct rng *r)
{
    uint32_t z = (r->state += 0x9e3779b9u);
    z = (z ^ (z >> 16)) * 0x85ebca6bu;
    z = (z ^ (z >> 13)) * 0xc2b2ae35u;
    return z ^ (z >> 16);
}

static size_t below(struct rng *r, size_t n)
{
    return rnd(r) % n;
}

static struct replay_record *record(uint8_t *data, size_t i)
{
    return (struct replay_record *)(data + i * RECORD);
}

/* Mostly a valid operation; now and then any byte, for the rejection path. */
static uint8_t draw_op(struct rng *r)
{
    if (below(r, 16) == 0) {
        return (uint8_t)rnd(r);
    }
    return (uint8_t)(1 + below(r, OP_COUNT - 1));
}

/* Any thread or one of the driver's; now and then a value the driver reads as any thread. */
static uint8_t draw_actor(struct rng *r)
{
    if (below(r, 16) == 0) {
        return (uint8_t)rnd(r);
    }
    return (uint8_t)below(r, REPLAY_THREADS + 1);
}

/* Mostly a slot the setup filled or one just above it; sometimes any slot at all. */
static uint16_t draw_slot(struct rng *r)
{
    switch (below(r, 8)) {
    case 0:
        return (uint16_t)below(r, REPLAY_TABLE_SLOTS + 2);
    case 1:
        return (uint16_t)rnd(r);
    default:
        return (uint16_t)below(r, REPLAY_CAP_PROCESS + 5);
    }
}

/* A field of another record, so that a value which already matters spreads. */
static uint32_t other_value(struct rng *r, uint8_t *data, size_t count)
{
    const struct replay_record *c = record(data, below(r, count));
    switch (below(r, 4)) {
    case 0:
        return c->slot;
    case 1:
        return c->a1;
    case 2:
        return c->a2;
    default:
        return c->a3;
    }
}

/*
 * An argument, from the ranges of every operation at once;
 * a table of what each operation reads would drift from rvuos/abi.h.
 */
static uint32_t draw_arg(struct rng *r, uint8_t *data, size_t count)
{
    switch (below(r, 12)) {
    case 0:
        return 0;
    case 1:
        return rnd(r);
    case 2:
    case 3:
        return (uint32_t)below(r, 10); /* a type, a rights mask or a region slot, all up to 8, or one past */
    case 4:
    case 5:
    case 6:
        return draw_slot(r);
    case 7:
        return (uint32_t)below(r, 64) * 4; /* an offset or a size on QEMU's grain */
    case 8:
        return (uint32_t)below(r, 32) * 32; /* the same on a 32-byte grain */
    case 9:
        return (uint32_t)below(r, 17) * (REPLAY_POOL_SIZE / 16); /* up to a pool the size of the setup's */
    case 10:
        return 1u << below(r, 32); /* notification bits */
    default:
        return count ? other_value(r, data, count) : rnd(r);
    }
}

/* A fresh value, a nudge, or a flipped bit. */
static uint32_t tweak_arg(struct rng *r, uint32_t old, uint8_t *data, size_t count)
{
    switch (below(r, 4)) {
    case 0:
        return old + 1 + (uint32_t)below(r, 8);
    case 1:
        return old - 1 - (uint32_t)below(r, 8);
    case 2:
        return old ^ (1u << below(r, 32));
    default:
        return draw_arg(r, data, count);
    }
}

static void draw_record(struct rng *r, struct replay_record *c, uint8_t *data, size_t count)
{
    c->op = draw_op(r);
    c->actor = draw_actor(r);
    c->slot = draw_slot(r);
    c->a1 = draw_arg(r, data, count);
    c->a2 = draw_arg(r, data, count);
    c->a3 = draw_arg(r, data, count);
}

static void mutate_field(struct rng *r, struct replay_record *c, uint8_t *data, size_t count)
{
    switch (below(r, 6)) {
    case 0:
        c->op = draw_op(r);
        break;
    case 1:
        c->actor = draw_actor(r);
        break;
    case 2:
        c->slot = draw_slot(r);
        break;
    case 3:
        c->a1 = tweak_arg(r, c->a1, data, count);
        break;
    case 4:
        c->a2 = tweak_arg(r, c->a2, data, count);
        break;
    default:
        c->a3 = tweak_arg(r, c->a3, data, count);
        break;
    }
}

/* Take record `at` out; returns the new count. */
static size_t remove_at(uint8_t *data, size_t count, size_t at)
{
    memmove(record(data, at), record(data, at + 1), (count - at - 1) * RECORD);
    return count - 1;
}

/* Put `c` at a random place; when the input is full, a random record makes way. */
static size_t insert(struct rng *r, uint8_t *data, size_t count, size_t room,
                     const struct replay_record *c)
{
    if (count == room) {
        count = remove_at(data, count, below(r, count));
    }
    size_t at = below(r, count + 1);
    memmove(record(data, at + 1), record(data, at), (count - at) * RECORD);
    *record(data, at) = *c;
    return count + 1;
}

size_t LLVMFuzzerCustomMutator(uint8_t *data, size_t size, size_t max_size, unsigned seed)
{
    struct rng r = { seed };
    size_t count = size / RECORD;
    size_t room = max_size / RECORD;
    struct replay_record tmp;

    if (room == 0) {
        return LLVMFuzzerMutate(data, size, max_size);
    }
    if (count == 0) {
        draw_record(&r, &tmp, data, 0);
        return insert(&r, data, 0, room, &tmp) * RECORD;
    }

    switch (below(&r, 12)) {
    case 0:
    case 1:
    case 2:
        return LLVMFuzzerMutate(data, size, max_size);
    case 3:
    case 4:
    case 5:
        mutate_field(&r, record(data, below(&r, count)), data, count);
        break;
    case 6:
    case 7:
        draw_record(&r, &tmp, data, count);
        count = insert(&r, data, count, room, &tmp);
        break;
    case 8:
        tmp = *record(data, below(&r, count));
        count = insert(&r, data, count, room, &tmp);
        break;
    case 9:
        /* Delete, unless it is the only record. */
        if (count > 1) {
            count = remove_at(data, count, below(&r, count));
        }
        break;
    case 10: {
        struct replay_record *a = record(data, below(&r, count));
        struct replay_record *b = record(data, below(&r, count));
        tmp = *a;
        *a = *b;
        *b = tmp;
        break;
    }
    default: {
        /* Move. */
        size_t from = below(&r, count);
        tmp = *record(data, from);
        count = remove_at(data, count, from);
        count = insert(&r, data, count, room, &tmp);
        break;
    }
    }
    return count * RECORD;
}

/*
 * A prefix of one input followed by a suffix of the other,
 * or the two interleaved so that a wait in one meets a signal in the other.
 */
size_t LLVMFuzzerCustomCrossOver(const uint8_t *data1, size_t size1,
                                 const uint8_t *data2, size_t size2,
                                 uint8_t *out, size_t max_out, unsigned seed)
{
    struct rng r = { seed };
    size_t n1 = size1 / RECORD;
    size_t n2 = size2 / RECORD;
    size_t room = max_out / RECORD;
    size_t i1 = 0;
    size_t i2 = 0;
    size_t n = 0;
    int interleave = below(&r, 2) == 0;

    if (!interleave) {
        n1 = below(&r, n1 + 1);
        i2 = below(&r, n2 + 1);
    }
    while (n < room && (i1 < n1 || i2 < n2)) {
        if (i1 < n1 && (i2 == n2 || !interleave || below(&r, 2) == 0)) {
            memcpy(out + n * RECORD, data1 + i1 * RECORD, RECORD);
            i1++;
        } else {
            memcpy(out + n * RECORD, data2 + i2 * RECORD, RECORD);
            i2++;
        }
        n++;
    }
    return n * RECORD;
}
