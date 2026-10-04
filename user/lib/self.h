#ifndef RVUOS_LIB_SELF_H
#define RVUOS_LIB_SELF_H

/*
 * What a process keeps of its own and hands out:
 * the slots of its table, the regions of its process, the bits of the notification it waits on,
 * units of time, and free memory.
 * The kernel keeps no allocator for any of them, as DESIGN.md intends,
 * so a process that builds others keeps them here.
 *
 * Nothing in user/lib/ keeps a global: every function works on what it is handed,
 * so the library serves a root task and its children alike, which run the same code but not with the same data;
 * the Makefile's no-globals check holds it to that.
 *
 * A call that fails returns the kernel's status, or KERR_LIMIT when what it hands out has run out,
 * and leaves the step that failed in what, for the caller to report.
 */

#include <stdint.h>

#include "rvuos.h"

/*
 * A block of memory: an Untyped of exactly the block, and what was made of it, a frame or a pool.
 * Revoking below the Untyped takes back what was made, wherever it was installed or given,
 * and leaves the Untyped free to make something else.
 */
struct block {
    uint32_t untyped;
    uint32_t made; /* the slot of what was made, or 0 when none is kept */
    uint32_t base, size;
};

#define SELF_SLOTS  128u /* the most slots of a table it keeps account of */
#define SELF_BLOCKS 24u  /* the most free blocks it keeps */
#define SELF_ROOM_UNIT 0x1000u /* what room for children's data is handed out in, see self_room */

struct self {
    /* Its capabilities, in its own table. */
    uint32_t table;       /* its own table, RIGHT_W: to delete and revoke */
    uint32_t process;     /* its own process, RIGHT_W: for its regions */
    uint32_t pool;        /* where its own objects come from */
    uint32_t inbox;       /* the notification it waits on, which its children's faults and pages signal */
    uint32_t code;        /* the frame of the code its children run */
    uint32_t timer_lines; /* the timer lines it may hand out */
    uint32_t timer_line_count; /* how many, from the first */
    uint32_t time;        /* the first core's units of time it may hand out */
    uint32_t debug;       /* the kernel's log, RIGHT_W at least, which its children are given with RIGHT_W alone */
    /* What is in use, a bit each. */
    uint32_t slot_first, slot_end; /* the slots it hands out, from first up to end */
    uint32_t slots[SELF_SLOTS / 32];
    uint32_t regions;
    uint32_t bits;
    uint32_t units[TIME_UNITS / 32];
    /* Free memory: Untypeds, each of a block, none overlapping. */
    struct block free[SELF_BLOCKS];
    uint32_t free_count;
    /* Room for children's data, if it was asked for: a frame, the region it is installed in, and a bit for each unit in use. */
    struct block room;
    uint32_t room_region, room_used;
    const char *what;
};

/* Returns the status of a system call that failed, with step in what. */
#define TRY(s, step, call)                                                                                             \
    do {                                                                                                               \
        uint32_t try_status = (call);                                                                                  \
        if (try_status != KERR_OK) {                                                                                   \
            (s)->what = (step);                                                                                        \
            return try_status;                                                                                         \
        }                                                                                                              \
    } while (0)

/* Returns the status of a call of the library's that failed, which named its step itself. */
#define PASS(call)                                                                                                     \
    do {                                                                                                               \
        uint32_t pass_status = (call);                                                                                 \
        if (pass_status != KERR_OK) {                                                                                  \
            return pass_status;                                                                                        \
        }                                                                                                              \
    } while (0)

/*
 * The root task's, from what the boot gave it: its slots past BOOT_CAP_COUNT, its regions past code and data,
 * the free RAM, and an inbox allocated from the boot pool.
 * It keeps units of the first core for itself, from the first, and hands out the rest.
 */
uint32_t self_root(struct self *s, uint32_t units);

/* Sets the lowest clear bit of a word's first count and says which; KERR_LIMIT, with what, when none is clear. */
uint32_t take_lowest(struct self *s, uint32_t *word, uint32_t count, uint32_t *i, const char *what);

/* A slot that holds nothing; KERR_LIMIT when every one is in use. */
uint32_t slot_new(struct self *s, uint32_t *slot);
/* A slot back, deleting what it holds; what was derived from that goes to its parent. */
uint32_t slot_free(struct self *s, uint32_t slot);
/* As slot_free, on the way out of a failure: what keeps the step that failed. */
void slot_back(struct self *s, uint32_t slot);
uint32_t slots_unused(const struct self *s);

/* A frame installed in a region of its own process, and the region back. */
uint32_t region_install(struct self *s, uint32_t frame, uint32_t rights, uint32_t *region);
uint32_t region_free(struct self *s, uint32_t region);

/* A bit of its inbox, to name one thing that signals it. */
uint32_t bit_new(struct self *s, uint32_t *bit);
void bit_free(struct self *s, uint32_t bit);

/*
 * An Irq on the first of its timer lines nothing is bound to, from pool, signalling ntfn, in a new slot.
 * The kernel knows which lines are bound and says so, so none is kept here:
 * a line is free again once the Irq bound to it is gone with its pool.
 */
uint32_t timer_bind(struct self *s, uint32_t pool, uint32_t ntfn, uint32_t *irq);
/* An Irq on a timer line of its own, which signals its inbox with the bits it is set with. */
uint32_t timer_new(struct self *s, uint32_t *irq);

/*
 * Room for children's data: a frame of size bytes, a power of two of at most 32 units, installed in a region of its own,
 * which child_new carves each child's data from once it is there,
 * rather than taking a block for each child that the parent would install in a region each.
 * So a parent spends one region on all its children's pages, which it sees through it.
 */
uint32_t self_room(struct self *s, uint32_t size);
/* The room back, its region and its block, once no child's data is in it. */
uint32_t self_room_free(struct self *s);
/* Size bytes of the room, a power of two of whole units at a multiple of their size, as a frame carved into a new slot. */
uint32_t room_take(struct self *s, uint32_t size, struct block *b);
/* The room's bytes back, once nothing is installed from them any more. */
void room_give(struct self *s, const struct block *b);

/* Units of the first core in a row, and back. */
uint32_t units_take(struct self *s, uint32_t count, uint32_t *first);
void units_give(struct self *s, uint32_t first, uint32_t count);

/*
 * Memory, as blocks halved out of the free ones: a block's size is a power of two and its base a multiple of it.
 * The halves of a block are never joined again, since their parent is deleted to keep its slot,
 * so a block given back stays as small as it was taken.
 */
/* An Untyped of size, from the smallest free block it fits, the lowest of those, at that block's base. */
uint32_t mem_take(struct self *s, uint32_t size, struct block *b);
/* An Untyped of exactly the block at base, from the free block that holds it. */
uint32_t mem_take_at(struct self *s, uint32_t base, uint32_t size, struct block *b);
/* Makes the block's Untyped into a frame or a pool, CAP_FRAME or CAP_POOL, in b->made. */
uint32_t mem_make(struct self *s, struct block *b, uint32_t type);
/* A frame of size, from mem_take. */
uint32_t mem_frame(struct self *s, uint32_t size, struct block *b);
/* The block back, with whatever was made of it, wherever that was installed or given. */
uint32_t mem_give(struct self *s, struct block *b);
/*
 * Copies len bytes at base out of free memory, as a loader may have left them there,
 * through a frame made of the free block that holds them for the moment; the block stays free.
 */
uint32_t mem_read(struct self *s, uint32_t base, void *buf, uint32_t len);
uint32_t mem_unused(const struct self *s);

#endif
