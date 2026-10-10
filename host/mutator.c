/*
 * A libFuzzer mutator that works on records rather than bytes.
 *
 * A byte flip lands outside a field's small range more often than inside,
 * so a mutation inserts, deletes, duplicates, swaps or moves a whole record, or sets one field,
 * and the crossover splices two inputs on record boundaries.
 * Most records it draws are typed, by the table of host/ops.h
 * and a guess of what each slot holds, which follows the records from the prologue:
 * a record invokes a capability of the type its operation wants, takes its sources from slots of their types,
 * and may use what an earlier record made, or come again with fresh slots.
 * Byte mutations and untyped draws stay, for the refusals the types leave out.
 *
 * The harness ignores a trailing partial record;
 * the record mutations drop it and the byte mutations keep it.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "layout.h"
#include "ops.h"
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

/* Mostly a valid operation; now and then a load or a store, see rvuos/replay.h, or any byte, for the rejection path. */
static uint8_t draw_op(struct rng *r)
{
    switch (below(r, 16)) {
    case 0:
        return (uint8_t)rnd(r);
    case 1:
        return below(r, 2) ? REPLAY_OP_LOAD : REPLAY_OP_STORE;
    default:
        return (uint8_t)(1 + below(r, OP_COUNT - 1));
    }
}

/*
 * An address for a load or a store: mostly in the free RAM the records make frames of,
 * at a block's edge, where a region begins or ends, or a word into it;
 * now and then in the root task's memory, the kernel's, or the clock's.
 */
static uint32_t draw_address(struct rng *r)
{
    static const uint32_t elsewhere[] = { RAM_BASE, BOOT_POOL_BASE, INPUT_BASE, COUNTER_ADDR };
    if (below(r, 8) == 0) {
        return elsewhere[below(r, sizeof(elsewhere) / sizeof(elsewhere[0]))] + (uint32_t)below(r, 16) * 4;
    }
    uint32_t block = 1u << below(r, 22);
    uint32_t at = FREE_RAM_BASE + (uint32_t)below(r, FREE_RAM_SIZE / block) * block;
    switch (below(r, 3)) {
    case 0:
        return at - 4;
    case 1:
        return at;
    default:
        return at + (uint32_t)below(r, 64) * 4;
    }
}

/* Any thread or one of the driver's; now and then a value the driver reads as any thread. */
static uint8_t draw_actor(struct rng *r)
{
    if (below(r, 16) == 0) {
        return (uint8_t)rnd(r);
    }
    return (uint8_t)below(r, REPLAY_THREADS + 1);
}

/*
 * Mostly a slot the setup filled or one just above it; sometimes any slot at all,
 * and sometimes one of a table mounted in the second table slot, which only an input mounts.
 */
static uint16_t draw_slot(struct rng *r)
{
    switch (below(r, 8)) {
    case 0:
        return (uint16_t)SLOT_IN(below(r, 2), below(r, REPLAY_TABLE_SLOTS + 2));
    case 1:
        return (uint16_t)rnd(r);
    default:
        return (uint16_t)below(r, REPLAY_CAP_RAM + 4);
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
 * An argument, from the ranges of every operation at once,
 * for the untyped draws, which reach what the table of host/ops.h rules out.
 */
static uint32_t draw_arg(struct rng *r, uint8_t *data, size_t count)
{
    switch (below(r, 13)) {
    case 12:
        return draw_address(r);
    case 0:
        return 0;
    case 1:
        return rnd(r);
    case 2:
    case 3:
        return (uint32_t)below(r, 14); /* a type, a rights mask, a region slot, a line or a unit, all up to 12, or one past */
    case 4:
    case 5:
    case 6:
        return draw_slot(r);
    case 7:
        return (uint32_t)below(r, 64) * 4; /* an offset or a size on QEMU's grain */
    case 8:
        return (uint32_t)below(r, 32) * 32; /* the same on a 32-byte grain */
    case 9:
        return (uint32_t)below(r, 17) * 0x100u; /* an offset or a size up to a page */
    case 10:
        return 1u << below(r, 32); /* notification bits, or a count of units, TIME_UNITS among them */
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

/* Put `c` at `at`; when the input is full, the last record makes way. */
static size_t insert_at(uint8_t *data, size_t count, size_t room, size_t at, const struct replay_record *c)
{
    if (count == room) {
        count--;
    }
    if (at > count) {
        at = count;
    }
    memmove(record(data, at + 1), record(data, at), (count - at) * RECORD);
    *record(data, at) = *c;
    return count + 1;
}

/* Put `c` at a random place; when the input is full, a random record makes way. */
static size_t insert(struct rng *r, uint8_t *data, size_t count, size_t room,
                     const struct replay_record *c)
{
    if (count == room) {
        count = remove_at(data, count, below(r, count));
    }
    return insert_at(data, count, room, below(r, count + 1), c);
}

/*
 * What the mutator guesses each slot of the driver's two tables holds, the root thread's and the second's,
 * which the third thread shares: a capability type, and for a table capability which of the two it names.
 */
enum { TABLES = 2, NOWHERE = 0xff };
struct shadow {
    uint8_t type[TABLES][REPLAY_TABLE_SLOTS];
    uint8_t names[TABLES][REPLAY_TABLE_SLOTS];
};

/* The table a record's thread uses; a record for whichever thread runs is guessed to be the root's. */
static unsigned table_of(uint8_t actor)
{
    return actor == 2 || actor == 3 ? 1 : 0;
}

static uint8_t slot_type(const struct shadow *s, unsigned t, uint32_t slot)
{
    return t < TABLES && slot < REPLAY_TABLE_SLOTS ? s->type[t][slot] : CAP_NONE;
}

static void put(struct shadow *s, unsigned t, uint32_t slot, uint8_t type, uint8_t names)
{
    if (t < TABLES && slot < REPLAY_TABLE_SLOTS) {
        s->type[t][slot] = type;
        s->names[t][slot] = names;
    }
}

/* The record's effect on the guess, as if it succeeded wherever the types fit. */
static void follow(struct shadow *s, const struct replay_record *c)
{
    const struct op_info *info = op_info(c->op);
    unsigned t = table_of(c->actor);
    if (info == NULL || info->cap == CAP_NONE || slot_type(s, t, c->slot) != info->cap) {
        return;
    }
    uint32_t a[3] = { c->a1, c->a2, c->a3 };
    uint8_t made = info->makes != MAKES_A1 ? info->makes : c->a1 < 0x100 ? (uint8_t)c->a1 : CAP_NONE;
    uint8_t inv = s->names[t][c->slot];
    /* A bind takes the invoked slot, which may receive the Irq. */
    if (c->op == OP_IRQ_BIND) {
        put(s, t, c->slot, CAP_NONE, NOWHERE);
    }
    for (unsigned i = 0; i < 3; i++) {
        switch (info->arg[i]) {
        case ARG_DST:
            if (made != CAP_NONE && slot_type(s, t, a[i]) == CAP_NONE) {
                put(s, t, a[i], made, NOWHERE);
            }
            break;
        case ARG_DST_INVOKED:
            /* A copy, a derive or a move: a2 is the source, in the caller's table. */
            if (inv != NOWHERE && slot_type(s, inv, a[i]) == CAP_NONE && slot_type(s, t, a[1]) != CAP_NONE) {
                put(s, inv, a[i], s->type[t][a[1]], s->names[t][a[1]]);
                if (c->op == OP_CAP_MOVE) {
                    put(s, t, a[1], CAP_NONE, NOWHERE);
                }
            }
            break;
        case ARG_SLOT_INVOKED:
            if (c->op == OP_CAP_DELETE && inv != NOWHERE) {
                put(s, inv, a[i], CAP_NONE, NOWHERE);
            }
            break;
        default:
            break;
        }
    }
}

/* The guess as the records begin: the boot capabilities, then the prologue of rvuos/replay.h. */
static const struct shadow *shadow_start(void)
{
    static struct shadow start;
    static int ready;
    static const uint8_t boot[BOOT_CAP_COUNT] = {
        [BOOT_CAP_CAPTABLE] = CAP_CAPTABLE, [BOOT_CAP_PROCESS] = CAP_PROCESS,
        [BOOT_CAP_THREAD] = CAP_THREAD, [BOOT_CAP_POOL] = CAP_POOL, [BOOT_CAP_DEBUG] = CAP_DEBUG,
        [BOOT_CAP_CODE] = CAP_FRAME, [BOOT_CAP_DATA] = CAP_FRAME, [BOOT_CAP_FREE_RAM] = CAP_UNTYPED,
        [BOOT_CAP_INPUT] = CAP_FRAME, [BOOT_CAP_IRQ_LINES] = CAP_IRQ_LINE, [BOOT_CAP_UART] = CAP_FRAME,
        [BOOT_CAP_LOG] = CAP_FRAME, [BOOT_CAP_TIMER_LINES] = CAP_IRQ_LINE, [BOOT_CAP_CLOCK] = CAP_CLOCK,
        [BOOT_CAP_TIME] = CAP_TIME, [BOOT_CAP_ROOT_RAM] = CAP_UNTYPED, [BOOT_CAP_POOL_RAM] = CAP_UNTYPED,
    };
    if (ready) {
        return &start;
    }
    memset(start.type, CAP_NONE, sizeof(start.type));
    memset(start.names, NOWHERE, sizeof(start.names));
    memcpy(start.type[0], boot, sizeof(boot));
    start.names[0][BOOT_CAP_CAPTABLE] = 0;
    for (size_t i = 0; i < REPLAY_PROLOGUE_COUNT; i++) {
        follow(&start, &replay_prologue[i]);
        /* The table the prologue makes is the second thread's. */
        if (replay_prologue[i].op == OP_POOL_ALLOC && replay_prologue[i].a2 == REPLAY_CAP_TABLE) {
            start.names[0][REPLAY_CAP_TABLE] = 1;
        }
    }
    ready = 1;
    return &start;
}

/* The guess before record `at`. */
static void shadow_at(struct shadow *s, const uint8_t *data, size_t at)
{
    *s = *shadow_start();
    for (size_t i = 0; i < at; i++) {
        struct replay_record c;
        memcpy(&c, data + i * RECORD, RECORD);
        follow(s, &c);
    }
}

/* A slot of the table that holds a capability of the type, any type for CAP_NONE, or an empty one. */
static uint32_t pick_slot(struct rng *r, const struct shadow *s, unsigned t, uint8_t want, int filled)
{
    uint32_t seen = 0, choice = 0;
    for (uint32_t i = 0; t < TABLES && i < REPLAY_TABLE_SLOTS; i++) {
        uint8_t type = s->type[t][i];
        int fits = filled ? type != CAP_NONE && (want == CAP_NONE || type == want) : type == CAP_NONE;
        if (fits && below(r, ++seen) == 0) {
            choice = i;
        }
    }
    return seen != 0 ? choice : draw_slot(r);
}

/* An argument of the kind the table says, for record `c`, whose earlier fields are drawn already. */
static uint32_t draw_kind(struct rng *r, const struct shadow *s, const struct replay_record *c,
                          const struct op_info *info, unsigned i, uint8_t *data, size_t count)
{
    static const uint8_t allocs[] = { CAP_CAPTABLE, CAP_PROCESS, CAP_THREAD, CAP_NOTIFICATION };
    static const uint8_t rights[] = { RIGHT_ALL, RIGHT_R, RIGHT_R | RIGHT_W, RIGHT_R | RIGHT_X, RIGHT_W, RIGHT_X };
    static const uint32_t delays[] = { 0, 1, 100, 10000, 1000000 };
    unsigned t = table_of(c->actor);
    uint8_t inv = c->slot < REPLAY_TABLE_SLOTS && t < TABLES ? s->names[t][c->slot] : NOWHERE;
    if (below(r, 8) == 0) {
        return draw_arg(r, data, count);
    }
    switch (info->arg[i]) {
    case ARG_NONE:
        return 0;
    case ARG_DST:
        return pick_slot(r, s, t, CAP_NONE, 0);
    case ARG_DST_INVOKED:
        return inv != NOWHERE ? pick_slot(r, s, inv, CAP_NONE, 0) : draw_slot(r);
    case ARG_SLOT_INVOKED:
        return inv != NOWHERE ? pick_slot(r, s, inv, CAP_NONE, 1) : draw_slot(r);
    case ARG_ANY:
    case ARG_FRAME:
    case ARG_POOL:
    case ARG_NOTIFICATION:
    case ARG_THREAD:
    case ARG_CAPTABLE:
        return pick_slot(r, s, t, arg_type(info->arg[i]), 1);
    case ARG_TYPE:
        if (c->op == OP_UNTYPED_RETYPE) {
            return below(r, 2) ? CAP_FRAME : CAP_POOL;
        }
        return allocs[below(r, sizeof(allocs))];
    case ARG_ALLOC:
        switch (c->a1) {
        case CAP_CAPTABLE:
            return below(r, 2) ? 1 + (uint32_t)below(r, 16) : 1u << below(r, 8);
        case CAP_PROCESS:
            return pick_slot(r, s, t, CAP_CAPTABLE, 1);
        case CAP_THREAD:
            return pick_slot(r, s, t, CAP_PROCESS, 1);
        default:
            return 0;
        }
    case ARG_RIGHTS:
        return rights[below(r, sizeof(rights))];
    case ARG_REGION:
        return (uint32_t)below(r, PROCESS_REGION_SLOTS + 1);
    case ARG_MOUNT:
        return (uint32_t)below(r, PROCESS_TABLES + 1);
    case ARG_OFFSET:
        return below(r, 2) ? (uint32_t)below(r, 16) : (uint32_t)below(r, 64) << below(r, 16);
    case ARG_SIZE:
        return 1u << below(r, 23);
    case ARG_COUNT:
        return below(r, 2) ? 1 : 1u << below(r, 7);
    case ARG_BITS:
        return below(r, 4) ? 1u << below(r, 32) : rnd(r);
    case ARG_LINE:
        return (uint32_t)below(r, IRQ_LINES + TIMER_LINES + 1);
    case ARG_DELAY:
        return delays[below(r, sizeof(delays) / sizeof(delays[0]))];
    case ARG_FLAGS:
        return below(r, 8) == 0 ? rnd(r) : (uint32_t)below(r, 2);
    case ARG_STEPS:
        return below(r, 8) == 0 ? 0 : 1 + (uint32_t)below(r, 16);
    case ARG_ADDRESS:
        return draw_address(r);
    default:
        return draw_arg(r, data, count);
    }
}

/* The operations invoked on a type, into ops; returns how many. */
static size_t ops_on(uint8_t type, uint8_t ops[OP_COUNT])
{
    size_t n = 0;
    for (uint8_t op = 1; op < OP_COUNT; op++) {
        if (op_table[op].name != NULL && op_table[op].cap == type) {
            ops[n++] = op;
        }
    }
    return n;
}

/* The arguments of a record whose actor, operation and slot are set, by the table. */
static void draw_args(struct rng *r, const struct shadow *s, struct replay_record *c, uint8_t *data, size_t count)
{
    const struct op_info *info = op_info(c->op);
    uint32_t *a[3] = { &c->a1, &c->a2, &c->a3 };
    for (unsigned i = 0; i < 3; i++) {
        *a[i] = info != NULL ? draw_kind(r, s, c, info, i, data, count) : draw_arg(r, data, count);
    }
}

/* A typed record: an operation on a capability the thread's table is guessed to hold, or a load or a store. */
static void typed_record(struct rng *r, const struct shadow *s, struct replay_record *c, uint8_t *data, size_t count)
{
    uint8_t ops[OP_COUNT];
    c->actor = draw_actor(r);
    unsigned t = table_of(c->actor);
    if (below(r, 8) == 0) {
        c->op = below(r, 2) ? REPLAY_OP_LOAD : REPLAY_OP_STORE;
        c->slot = 0;
    } else {
        c->slot = (uint16_t)pick_slot(r, s, t, CAP_NONE, 1);
        size_t n = ops_on(slot_type(s, t, c->slot), ops);
        c->op = n != 0 ? ops[below(r, n)] : draw_op(r);
    }
    draw_args(r, s, c, data, count);
}

/* Set one field of record `at` by the table, from the guess before it. */
static void typed_field(struct rng *r, uint8_t *data, size_t count, size_t at)
{
    struct shadow s;
    shadow_at(&s, data, at);
    struct replay_record *c = record(data, at);
    const struct op_info *info = op_info(c->op);
    unsigned field = (unsigned)below(r, 4);
    if (info == NULL) {
        mutate_field(r, c, data, count);
    } else if (field == 0) {
        if (info->cap != CAP_NONE) {
            c->slot = (uint16_t)pick_slot(r, &s, table_of(c->actor), info->cap, 1);
        }
    } else {
        uint32_t *a[3] = { &c->a1, &c->a2, &c->a3 };
        *a[field - 1] = draw_kind(r, &s, c, info, field - 1, data, count);
    }
}

/* The first slot a record puts a capability in, and the table, or 0 when it puts none. */
static int made_slot(const struct replay_record *c, uint32_t *slot)
{
    const struct op_info *info = op_info(c->op);
    uint32_t a[3] = { c->a1, c->a2, c->a3 };
    for (unsigned i = 0; info != NULL && i < 3; i++) {
        if (info->arg[i] == ARG_DST) {
            *slot = a[i];
            return 1;
        }
    }
    return 0;
}

/*
 * After a record that makes a capability, somewhere later, one that uses it:
 * invokes it, by a thread of the same table, as the guess after the maker has it.
 */
static size_t use_made(struct rng *r, uint8_t *data, size_t count, size_t room)
{
    size_t k = below(r, count);
    uint32_t slot;
    if (!made_slot(record(data, k), &slot)) {
        return count;
    }
    struct shadow s;
    shadow_at(&s, data, k + 1);
    unsigned t = table_of(record(data, k)->actor);
    uint8_t ops[OP_COUNT];
    size_t n = ops_on(slot_type(&s, t, slot), ops);
    if (n == 0) {
        return count;
    }
    struct replay_record c = { .op = ops[below(r, n)], .actor = record(data, k)->actor, .slot = (uint16_t)slot };
    draw_args(r, &s, &c, data, count);
    return insert_at(data, count, room, k + 1 + below(r, count - k), &c);
}

/*
 * A record again, a few times, each into fresh slots;
 * half the time each invokes what the one before it made, as a chain of splits does.
 */
static size_t repeat(struct rng *r, uint8_t *data, size_t count, size_t room)
{
    size_t k = below(r, count);
    struct replay_record c = *record(data, k);
    const struct op_info *info = op_info(c.op);
    if (info == NULL) {
        return count;
    }
    int chain = below(r, 2) == 0;
    struct shadow s;
    shadow_at(&s, data, k + 1);
    unsigned t = table_of(c.actor);
    size_t times = 2 + below(r, 7);
    for (size_t j = 0; j < times && count < room; j++) {
        uint32_t *a[3] = { &c.a1, &c.a2, &c.a3 };
        uint32_t made;
        if (chain && made_slot(&c, &made)) {
            c.slot = (uint16_t)made;
        }
        for (unsigned i = 0; i < 3; i++) {
            if (info->arg[i] == ARG_DST) {
                *a[i] = pick_slot(r, &s, t, CAP_NONE, 0);
            }
        }
        follow(&s, &c);
        count = insert_at(data, count, room, k + 1 + j, &c);
    }
    return count;
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
        typed_record(&r, shadow_start(), &tmp, data, 0);
        return insert(&r, data, 0, room, &tmp) * RECORD;
    }

    switch (below(&r, 16)) {
    case 0:
    case 1:
        return LLVMFuzzerMutate(data, size, max_size);
    case 2:
        mutate_field(&r, record(data, below(&r, count)), data, count);
        break;
    case 3:
    case 4:
        typed_field(&r, data, count, below(&r, count));
        break;
    case 5:
        draw_record(&r, &tmp, data, count);
        count = insert(&r, data, count, room, &tmp);
        break;
    case 6:
    case 7:
    case 8: {
        size_t at = below(&r, count + 1);
        struct shadow s;
        shadow_at(&s, data, at);
        typed_record(&r, &s, &tmp, data, count);
        count = insert_at(data, count, room, at, &tmp);
        break;
    }
    case 9:
    case 10:
        count = use_made(&r, data, count, room);
        break;
    case 11:
        count = repeat(&r, data, count, room);
        break;
    case 12:
        tmp = *record(data, below(&r, count));
        count = insert(&r, data, count, room, &tmp);
        break;
    case 13:
        /* Delete, unless it is the only record. */
        if (count > 1) {
            count = remove_at(data, count, below(&r, count));
        }
        break;
    case 14: {
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
