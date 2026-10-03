#ifndef RVUOS_LIBTEST_H
#define RVUOS_LIBTEST_H

/*
 * The library's test, see root.c: what the root task and its children agree on,
 * which is only their pages, since the library hands out their slots, regions and bits.
 */

#include "lib/chan.h"
#include "lib/child.h"

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

void peer_main(struct child_page *page);
void fault_main(struct child_page *page);
void server_main(struct child_page *page);
void client_main(struct child_page *page);

#endif
