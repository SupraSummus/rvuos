#ifndef RVUOS_LIBTEST_H
#define RVUOS_LIBTEST_H

/*
 * The library's test, see root.c: what the root task and its children agree on,
 * which is only their pages, since the library hands out their slots, regions and bits.
 */

#include "lib/bulk.h"
#include "lib/chan.h"
#include "lib/child.h"
#include "lib/event.h"
#include "lib/lock.h"
#include "lib/seqlock.h"
#include "lib/wait.h"

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
#define CLIENT_STEP_DIRTY 3u /* its channel held bytes when it started; detail is the offset in a ring */
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
    uint32_t named;           /* it takes the lock below instead, over the same word and notification */
    struct named_lock name;
    uint32_t count;           /* the address of the count */
    volatile uint32_t waited; /* the rounds it found the lock held */
};

/*
 * An event the root task waits for, and a child, the setter, sets EVENT_SETS times, each once the root task asks:
 * it sleeps until the asks pass the sets it made, so that the root task is waiting when the set comes.
 * Before it the root task sets the event itself, and finds a set before the wait kept, two sets before one wait one,
 * and the bits a wait before may leave, a set's and the other timer's, passed over.
 */
#define EVENT_SETS     20u
#define EVENT_SLEEP_US 500u
#define EVENT_SHORT_US 3000u    /* a wait nothing but its deadline ends */
#define EVENT_LONG_US  1000000u /* a wait a set ends */
enum {
    SETTER_DONE = CHILD_RUNNING + 1,
};
struct evented {
    uint32_t word;
    volatile uint32_t asked; /* the root task's: the sets it has asked for */
};
struct setter_page {
    struct child_page c;
    struct event event; /* its notification may be signalled and not waited on, and it has no timer */
    uint32_t asked;     /* the address of the count of asks */
};

/*
 * A snapshot of SNAP_WORDS words, each its version, which a thread of the root task's publishes over and over,
 * on the second core where there is one, while a child holding it read only copies it out over and over,
 * finding every copy whole and none older than the one before.
 * The root task then stops the writer, halfway through a write as like as not, and finds the snapshot whole still.
 * The reader counts the copies it read while the writer was in the middle of a write, as the writer says in the page,
 * and the tries a write was published under; a run with neither tried nothing, and fails.
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
    volatile uint32_t stop;    /* the root task's: 1 once the reader is to stop */
    volatile uint32_t writing; /* the writer's: 1 while it is in the middle of a write */
    /* The reader's: whole copies, those read while the writer was writing, and tries a write was published under. */
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
 * Bulk data, lib/bulk.h: a lender with a connection for each of BULK_PEERS borrowers, asked and answered BULK_ROUNDS times.
 * Each borrower asks its requests in order in the buffers to fill it was lent, and hands each back filled;
 * the lender, having checked every byte where it lies, answers it in a buffer lent to read and gives the request's buffer
 * back to fill again. The borrower checks every byte of each answer where it lies, and that it may write a request
 * and not an answer, and gives the answer's buffer back.
 * Since the lender writes nothing of a connection's but answers, a borrower that starts finds its blocks zeroed,
 * nothing of the last borrower's; and the lender, which keeps a buffer to read for itself, finds every buffer free again.
 * A borrower that forges also hands over BULK_FORGED references that name nothing it holds,
 * which the lender refuses and goes on; one told to breach stores into an answer, which must stop it and nothing else;
 * and one told to flood moves its ring's count of references a billion on, which must keep the lender from no other.
 */
#define BULK_PEERS  2u
#define BULK_ROUNDS 24u
#define BULK_SIZE   128u
#define BULK_READS  4u
#define BULK_FILLS  4u
#define BULK_FORGED 4u

static inline uint8_t bulk_byte(uint32_t seed, uint32_t n, uint32_t i)
{
    return (uint8_t)(seed * 31u + n * 7u + i * 13u + 5u);
}

/* Request n is filled from its offset with its length of bytes, and answered so; neither runs past a buffer. */
static inline uint32_t request_off(uint32_t n)
{
    return n % 8u;
}
static inline uint32_t request_len(uint32_t n)
{
    return 1u + n * 29u % (BULK_SIZE - 8u);
}
static inline uint32_t answer_off(uint32_t n)
{
    return n % 4u;
}
static inline uint32_t answer_len(uint32_t n)
{
    return 1u + n * 37u % (BULK_SIZE - 4u);
}
/* What a borrower fills a request with, beside the seed of its connection's answers. */
#define REQUEST_SEED(seed) ((seed) + 1000u)

#define LENDER_STEP_BYTES 1u /* a request came back with other bytes; detail is the connection */
#define LENDER_STEP_LEND  2u /* a buffer it held would not go; detail is the connection */
#define LENDER_STEP_BOOKS 3u /* a connection connected anew had a buffer someone held; detail is the connection */
struct lender_page {
    struct child_page c;
    struct bulk_end ends[BULK_PEERS];
    uint32_t seed[BULK_PEERS]; /* the root task's: what each connection's bytes start from */
    /* Each connection's requests back and checked, and answers lent; and the closes it let go of. */
    volatile uint32_t asked[BULK_PEERS], answered[BULK_PEERS], let_go;
};

enum {
    BORROWER_DONE = CHILD_RUNNING + 1, /* filled every request and read every answer as it was written */
    BORROWER_BREACHED,                 /* stored into an answer */
    BORROWER_FLOODED,                  /* moved its ring's count on */
};
#define BORROWER_STEP_DIRTY 1u /* its blocks held bytes when it started; detail is the offset in the block */
#define BORROWER_STEP_BYTES 2u /* an answer held other bytes; detail is its round */
#define BORROWER_STEP_ROOM  3u /* it may write an answer, or may not a request; detail is the buffer */
#define BORROWER_STEP_HAND  4u /* a buffer it held would not go back; detail is the buffer */
struct borrower_page {
    struct child_page c;
    struct bulk_end link;
    uint32_t seed;   /* its connection's */
    uint32_t forge;  /* hands over references it does not hold, too */
    uint32_t breach; /* stores into its first answer */
    uint32_t flood;  /* moves its ring's count of references on, and does nothing else */
    volatile uint32_t filled, read;
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

/*
 * A mover: a child that moves its own table to one twice as large, from the pool its parent let it have, self_migrate.
 * Its capabilities keep their numbers, so its reports reach its parent through the slot they did,
 * and the first slot past the old table's end holds a notification it makes and signals there.
 */
#define MOVER_TABLE 24u
enum {
    MOVER_MOVED = CHILD_RUNNING + 1, /* its table is the new one, and a slot past the old end works */
};
#define MOVER_STEP_MOVE 1u /* self_migrate failed, or handed out no more slots; detail is the status */
#define MOVER_STEP_FAR  2u /* the first slot past the old end did not hold what it made there; detail is the status */
struct mover_page {
    struct child_page c;
    struct child_own own;
};

void peer_main(struct child_page *page);
void fault_main(struct child_page *page);
/* A child that answers each check its parent asks, and one that spins and answers none. */
void answer_main(struct child_page *page);
void spin_main(struct child_page *page);
void server_main(struct child_page *page);
void client_main(struct child_page *page);
void locker_main(struct child_page *page);
void setter_main(struct child_page *page);
void passer_main(struct child_page *page);
void lender_main(struct child_page *page);
void borrower_main(struct child_page *page);
void snap_reader_main(struct child_page *page);
void builder_main(struct child_page *page);
void mover_main(struct child_page *page);

#endif
