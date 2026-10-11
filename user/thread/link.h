#ifndef RVUOS_THREAD_LINK_H
#define RVUOS_THREAD_LINK_H

/*
 * IEEE 802.15.4's MAC over radio.c for a PAN without beacons, as far as a node of rvuos needs it:
 * a data frame goes after unslotted CSMA-CA, and again until acknowledged, up to macMaxFrameRetries times;
 * a frame for this node that asks is acknowledged, and a repeat, told by its sequence number, is not taken twice.
 * Each neighbour's frames are counted, and from them ETX, the transmissions a frame to it takes.
 *
 * The driver's thread waits on the radio's line and on the link's timer line and hands it the bits that came;
 * what is shorter than the kernel's tick is waited for on the clock's counter, receiving meanwhile.
 * Nothing here keeps a global: the link's state is handed in.
 */

#include <stdbool.h>
#include <stdint.h>

#include "mac154.h"
#include "radio.h"

#define LINK_BROADCAST 0xffffu
#define LINK_PAYLOAD_MAX (RADIO_PSDU_MAX - RADIO_FCS - MAC154_DATA_HEADER)
/* The neighbours it keeps counts of, and the frames it holds for the driver until taken. */
#define LINK_NEIGHBORS 8u
#define LINK_QUEUE     4u
/* ETX in fixed point: this is one transmission. */
#define LINK_ETX_ONE 128u

/* How a frame's sending ended. */
enum link_result {
    LINK_ACKED,  /* acknowledged */
    LINK_SENT,   /* sent, to everyone, so asking nothing */
    LINK_NO_ACK, /* not acknowledged, however often sent */
    LINK_BUSY,   /* never sent: the channel was busy at every assessment, a failure of access to the channel */
    LINK_RESULTS
};

/* A neighbour, by its short address, and what its frames and the frames to it did. */
struct link_neighbor {
    uint16_t addr;
    bool used;
    bool seq_known;
    uint8_t seq;       /* the sequence number of its last data frame, which tells a repeat */
    uint32_t seen;     /* when it was last heard or sent to, by the link's own count, which picks one to replace */
    uint32_t heard, repeats, lqi;          /* its data frames, the repeats among them, and their mean link quality */
    uint32_t frames, acked, failed, tries; /* the frames to it, acknowledged or not, and the transmissions they took */
    uint32_t etx;      /* the mean transmissions a frame to it takes, LINK_ETX_ONE for one, 0 until one is known */
};

/* A data frame for this node, or for everyone, as it is held until taken. */
struct link_frame {
    uint16_t src, dst;
    uint8_t seq;
    uint32_t lqi;
    uint32_t length;
    uint8_t payload[LINK_PAYLOAD_MAX];
};

/* What the link did, for the log; how each frame ended, link_done tells. */
struct link_counts {
    uint32_t frames, tries, busy; /* frames given it to send, their transmissions, and assessments that found it busy */
    uint32_t acks_sent, bad_fcs, stray_acks, others, dropped;
};

enum link_state { LINK_IDLE, LINK_BACKOFF, LINK_SENDING };

struct link {
    struct radio radio;
    uint32_t counter;          /* the clock's counter, counting microseconds, where it is mapped */
    uint32_t irq, irq_bit;     /* the radio's line, and its bit */
    uint32_t timer, timer_bit; /* the link's timer line, and its bit */
    uint16_t pan, addr;
    uint32_t random;
    uint8_t seq;               /* the next data frame's sequence number */
    uint32_t seen;
    enum link_state state;
    /* The frame being sent, and how far its sending has come. */
    struct {
        uint8_t frame[RADIO_PSDU_MAX - RADIO_FCS];
        uint32_t length;
        uint16_t dst;
        uint8_t seq;
        bool ack;
        uint32_t backoffs, exponent, tries; /* 802.15.4's NB and BE, and the transmissions so far */
        uint32_t due;                       /* the counter's value at the end of the backoff */
    } out;
    bool done;
    enum link_result result;
    struct link_frame queue[LINK_QUEUE];
    uint32_t queue_head, queue_count;
    struct link_neighbor neighbors[LINK_NEIGHBORS];
    struct link_counts counts;
    uint32_t fault; /* the status of the first call to the kernel that failed, or KERR_OK */
};

/*
 * The link of a node, addr in pan, over the radio at base, off the air:
 * counter is where the clock's counter is mapped, which counts microseconds;
 * irq is the radio's line and timer a timer line, each signalling the driver's notification with its bit;
 * seed starts the backoffs' random numbers, and should differ from node to node.
 */
void link_init(struct link *l, uint32_t base, uint32_t counter, uint32_t irq, uint32_t irq_bit, uint32_t timer,
               uint32_t timer_bit, uint16_t pan, uint16_t addr, uint32_t seed);

/* Stirs bits into the backoffs' random numbers, such as what a node measured that another did not. */
void link_stir(struct link *l, uint32_t bits);

/* The power it sends at, as radio_power picks it. */
int32_t link_power(struct link *l, int32_t dbm);

/* Receiving on channel from now on. */
void link_listen(struct link *l, uint32_t channel);

/* Off the air, its lines quiet. */
void link_off(struct link *l);

/*
 * Starts sending payload, length bytes, to dst, asking for an acknowledgement unless dst is LINK_BROADCAST.
 * False if a frame is still being sent or the payload does not fit.
 */
bool link_send(struct link *l, uint16_t dst, const uint8_t *payload, uint32_t length);

/* After a wait: what the bits of the radio's line and of the timer line, if they came, end. */
void link_wake(struct link *l, uint32_t bits);

/* Whether the frame link_send started has finished, once, and how. */
bool link_done(struct link *l, enum link_result *result);

/* A data frame received for this node or everyone, oldest first, if one is held. */
bool link_take(struct link *l, struct link_frame *f);

#endif
