#ifndef RVUOS_LIBTEST_H
#define RVUOS_LIBTEST_H

/*
 * The library's test, see root.c: what the root task and its children agree on,
 * which is only their pages, since the library hands out their slots, regions and bits.
 */

#include "lib/chan.h"
#include "lib/child.h"
#include "lib/lock.h"
#include "lib/seqlock.h"

/* Each peer sends PACKETS packets of 1 to PACKET_MAX bytes, through rings of a few slots. */
#define PACKETS    40u
#define PACKET_MAX 60u
#define LINK_SIZE  0x400u

/* Lengths from 1 to PACKET_MAX in no order, so that full slots come round too. */
static inline uint32_t packet_len(uint32_t n)
{
    return 1u + n * 23u % PACKET_MAX;
}

static inline uint8_t packet_byte(uint32_t n, uint32_t i)
{
    return (uint8_t)(n * 7u + i * 13u + 1u);
}

enum {
    PEER_DONE = CHILD_RUNNING + 1, /* sent its packets, and received the other's as they were sent */
};
#define PEER_STEP_LENGTH 1u /* a packet of another length came; detail is its number */
#define PEER_STEP_BYTES  2u /* or with other bytes */

struct peer_page {
    struct child_page c;
    struct chan_end link;
    uint32_t sends_first;
};

/* A child that stores to an address it has no region over, which must stop it and nothing else. */
enum {
    FAULT_BREACHED = CHILD_RUNNING + 1,
};
struct fault_page {
    struct child_page c;
    uint32_t address;
};

/*
 * A hub: a server that sends each packet back on the channel it came on, and clients that each send it PACKETS,
 * one at a time, and check what comes back.
 */
#define HUB_CLIENTS   2u
#define HUB_CHAN_SIZE 0x400u
enum {
    CLIENT_DONE = CHILD_RUNNING + 1, /* had every packet back as it sent it */
};
struct server_page {
    struct child_page c;
    struct chan_end ends[HUB_CLIENTS];
    volatile uint32_t let_go; /* how many times it let go of a client */
};
struct client_page {
    struct child_page c;
    struct chan_end link;
    uint32_t seed; /* what its packets' bytes start from */
};

/*
 * Holders of a lock, each adding one to a count they share LOCK_ROUNDS times:
 * it reads the count under the lock and writes it back one more, every other round sleeping between the two,
 * so that the others find the lock held, and a count two held at once would come out short.
 */
#define LOCKERS       3u
#define LOCK_ROUNDS   20u
#define LOCK_SLEEP_US 1000u
enum {
    LOCKER_DONE = CHILD_RUNNING + 1,
};
#define LOCKER_STEP_TAKE 1u /* a take failed; detail is the round */
struct locked {
    uint32_t word;
    volatile uint32_t count;
};
struct locker_page {
    struct child_page c;
    struct lock lock;         /* which no other locker can write */
    uint32_t count;           /* the address of the count */
    volatile uint32_t waited; /* the rounds it found the lock held */
};

/*
 * A snapshot of SNAP_WORDS words, each its version, which a thread of the root task's publishes over and over,
 * on the second core where there is one, while a child holding it read only copies it out over and over,
 * finding every copy whole and none older than the one before.
 * The root task then stops the writer, halfway through a write as like as not, and finds the snapshot whole still.
 * The reader counts the copies it read with the writer halfway, the count odd, and the tries a write began under;
 * a run with neither tried nothing, and fails.
 */
#define SNAP_WORDS   8u
#define SNAP_US      50000u /* how long the writer writes */
#define SNAP_STOP_US 3000u  /* how long the writer may go on once its units are taken: the rest of its turn */
enum {
    SNAP_READ = CHILD_RUNNING + 1, /* the reader stopped as asked */
};
#define SNAP_STEP_TORN  1u /* a copy whose words are not all its version; detail is the version */
#define SNAP_STEP_OLDER 2u /* a copy older than the one before; detail is its version */
struct snap_page {
    struct child_page c;
    struct seqlock lock;
    volatile uint32_t stop; /* the root task's: 1 once the reader is to stop */
    /* The reader's: whole copies, those read with the writer halfway, and tries a write began under. */
    volatile uint32_t reads, halfway, again;
};

/*
 * A buffer of one word handed between two children PASSES times, taken from the one before it is given to the other.
 * Each, told it holds it, checks that the word is the last pass and writes its own, with no fence, MANUAL.md section 5.7;
 * the last, told it holds it no longer, stores to it anyway, which must stop it and nothing else.
 */
#define PASSES    3u
#define PASS_GONE 0xffffffffu
enum {
    PASSER_BREACHED = CHILD_RUNNING + 1, /* stored to the buffer it no longer holds */
    PASSER_HELD,                         /* PASSER_HELD + n: wrote the buffer for pass n */
};
#define PASSER_STEP_BUFFER 1u /* the word was not the last pass; detail is what it was */
struct passer_page {
    struct child_page c;
    uint32_t buffer;        /* its address */
    volatile uint32_t pass; /* the root task's, before each tell: the pass it holds the buffer for, or PASS_GONE */
};

/*
 * A builder: a child that builds a thread of its own from what its parent let it have, child_give_own.
 * The thread sleeps on a timer of its own, says so in the page and stops at a breakpoint,
 * a fault the parent hears as the builder's.
 * The builder also finds that its account gives no more than it was given: a timer line, then none, and no unit.
 */
#define OWN_POOL     0x1000u
#define OWN_LINES    2u
#define OWN_UNITS    2u
#define OWN_SLOTS    8u
#define OWN_SLEEP_US 1000u
#define BUILDER_TABLE 24u
enum {
    BUILDER_BUILT = CHILD_RUNNING + 1, /* its thread is started, and its account gave no more than it was given */
};
#define BUILDER_STEP_BUILD 1u /* a call building the thread failed; detail is the status */
#define BUILDER_STEP_LIMIT 2u /* its account gave what it was not given, or not what it was; detail says which */
struct builder_page {
    struct child_page c;
    struct child_own own;
    uint32_t note, timer;      /* the thread's, which the builder made */
    volatile uint32_t worked;  /* the thread's: 1 once its timer woke it */
    uint8_t stack[1024] __attribute__((aligned(16))); /* the thread's */
};

void peer_main(struct child_page *page);
void fault_main(struct child_page *page);
/* A child that answers each check its parent asks, and one that spins and answers none. */
void answer_main(struct child_page *page);
void spin_main(struct child_page *page);
void server_main(struct child_page *page);
void client_main(struct child_page *page);
void locker_main(struct child_page *page);
void passer_main(struct child_page *page);
void snap_reader_main(struct child_page *page);
void builder_main(struct child_page *page);

#endif
