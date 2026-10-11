/*
 * IEEE 802.15.4's MAC over the nRF52840's radio; see link.h.
 *
 * Its numbers are 802.15.4-2006's for the 2.4 GHz O-QPSK PHY, its tables 70 and 86:
 * a symbol lasts 16 us and a backoff period 20 of them;
 * a frame waits macAckWaitDuration for its acknowledgement, 54 symbols,
 * a backoff period, the turnaround of 12, the synchronization header's 10 and six of the acknowledgement's octets;
 * the backoff's exponent goes from macMinBE, 3, to macMaxBE, 5, the channel is assessed at most macMaxCSMABackoffs, 4,
 * times again, and a frame is sent again at most macMaxFrameRetries, 3, times.
 *
 * A wait on the counter receives meanwhile and acknowledges what asks,
 * so a neighbour that sends during a backoff or while this node waits for an acknowledgement is answered.
 */

#include "link.h"

#include "lib/libc.h"
#include "rvuos.h"

#define BACKOFF_PERIOD_US 320u
#define MIN_BE            3u
#define MAX_BE            5u
#define MAX_CSMA_BACKOFFS 4u
#define MAX_FRAME_RETRIES 3u
#define ACK_WAIT_US       864u
/*
 * An acknowledgement starts aTurnaroundTime, 12 symbols, after the frame it acknowledges, and not before:
 * its sender turns round to receive meanwhile, and the C6 hears none that comes earlier.
 * The radio ramps up to sending in 40 us, so it is told to send that much before.
 * Once told, the acknowledgement takes the ramp, then its 11 octets on the air, 352 us; bounded well past them.
 */
#define TURNAROUND_US 192u
#define RAMP_US       40u
#define ACK_SEND_US   2000u
/*
 * A backoff at least this long is slept through on the timer line, which fires up to a tick past its delay,
 * and its last part waited for on the counter.
 */
#define SLEEP_MIN_US 2000u
/* The assessment finds the channel busy at -75 dBm or more, 10 dB above the sensitivity 802.15.4 asks of a receiver. */
#define CCA_DBM (-75)
/* A frame never acknowledged counts in ETX as twice the transmissions it was given. */
#define ETX_NO_ACK (2u * (MAX_FRAME_RETRIES + 1u))
/* A mean moves by an eighth of the way to each new sample. */
#define MEAN_WEIGHT 8u
/* The random numbers' state, should a seed or the counter's bits make it zero, where xorshift would stay. */
#define RANDOM_NONZERO 0x9e3779b9u

static uint32_t now(const struct link *l)
{
    return rv_counter_low(l->counter);
}

/* Whether the counter's value t is still before due, as differences of the wrapping counter tell. */
static bool before(uint32_t t, uint32_t due)
{
    return (int32_t)(due - t) > 0;
}

static void call(struct link *l, uint32_t status)
{
    if (status != KERR_OK && l->fault == KERR_OK) {
        l->fault = status;
    }
}

static void arm(struct link *l)
{
    call(l, rv_irq_set(l->irq, l->irq_bit));
}

/* A random number for a backoff: xorshift32, stirred with the counter, whose low bits the wakes' timing moves. */
static uint32_t random_next(struct link *l)
{
    uint32_t x = l->random ^ now(l);
    if (x == 0) {
        x = RANDOM_NONZERO;
    }
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    l->random = x;
    return x;
}

static uint32_t mean(uint32_t m, uint32_t sample, bool first)
{
    return first ? sample : (m * (MEAN_WEIGHT - 1u) + sample) / MEAN_WEIGHT;
}

/* The neighbour of address addr, in a free slot if it is new, or in that of the one longest not heard or sent to. */
static struct link_neighbor *neighbor(struct link *l, uint16_t addr)
{
    struct link_neighbor *n = 0, *spare = 0;
    for (uint32_t i = 0; i < LINK_NEIGHBORS && n == 0; i++) {
        struct link_neighbor *c = &l->neighbors[i];
        if (c->used && c->addr == addr) {
            n = c;
        } else if (spare == 0 || (spare->used && (!c->used || l->seen - c->seen > l->seen - spare->seen))) {
            spare = c;
        }
    }
    if (n == 0) {
        n = spare;
        memset(n, 0, sizeof(*n));
        n->used = true;
        n->addr = addr;
    }
    n->seen = ++l->seen;
    return n;
}

/*
 * The acknowledgement of the frame numbered seq, which ended no later than the counter's value ended,
 * sent once its turnaround is over, and waited for; the radio receives again after it.
 * A frame is noticed some microseconds after it ends, a wake's or a wait's,
 * so the acknowledgement starts that much past the turnaround, well within the 54 symbols its sender waits.
 */
static void acknowledge(struct link *l, uint8_t seq, uint32_t ended)
{
    uint8_t ack[MAC154_ACK_LENGTH];
    uint32_t length = mac154_ack_frame(ack, seq);
    while (before(now(l), ended + TURNAROUND_US - RAMP_US)) {
    }
    radio_send_start(&l->radio, ack, length);
    uint32_t due = now(l) + ACK_SEND_US;
    bool gone;
    while (!(gone = radio_sent(&l->radio)) && before(now(l), due)) {
    }
    if (!gone) {
        radio_receive_start(&l->radio);
    }
    l->counts.acks_sent++;
}

/*
 * A frame heard, which ended no later than the counter's value ended:
 * acknowledged if it is a data frame for this node that asks, held if it is no repeat, counted otherwise.
 * True if it is the acknowledgement of the frame being sent.
 */
static bool heard(struct link *l, const struct radio_frame *f, uint32_t ended)
{
    if (!f->fcs_ok) {
        l->counts.bad_fcs++;
        return false;
    }
    if (f->length > 0 && (f->bytes[0] & MAC154_TYPE_MASK) == MAC154_TYPE_ACK) {
        if (l->state == LINK_SENDING && l->out.ack && mac154_acks(f->bytes, f->length, l->out.seq)) {
            return true;
        }
        l->counts.stray_acks++;
        return false;
    }
    struct mac154_data d;
    if (!mac154_data_read(f->bytes, f->length, &d) || d.pan != l->pan ||
        (d.dst != l->addr && d.dst != LINK_BROADCAST)) {
        l->counts.others++;
        return false;
    }
    /* First, as its sender waits only 54 symbols; the frame stays where the radio wrote it meanwhile. */
    if (d.ack_request && d.dst == l->addr) {
        acknowledge(l, d.seq, ended);
    }
    struct link_neighbor *n = neighbor(l, d.src);
    n->lqi = mean(n->lqi, f->lqi, n->heard == 0);
    n->heard++;
    if (n->seq_known && n->seq == d.seq) {
        n->repeats++;
        return false;
    }
    n->seq_known = true;
    n->seq = d.seq;
    if (l->queue_count == LINK_QUEUE || d.length > LINK_PAYLOAD_MAX) {
        l->counts.dropped++;
        return false;
    }
    struct link_frame *q = &l->queue[(l->queue_head + l->queue_count) % LINK_QUEUE];
    q->src = d.src;
    q->dst = d.dst;
    q->seq = d.seq;
    q->lqi = f->lqi;
    q->length = d.length;
    memcpy(q->payload, d.payload, d.length);
    l->queue_count++;
    return false;
}

/* A frame, if one has been received since the last; true if it acknowledges the frame being sent. */
static bool receive(struct link *l)
{
    struct radio_frame f;
    return radio_received(&l->radio, &f) && heard(l, &f, now(l));
}

static void finish(struct link *l, enum link_result result)
{
    l->counts.tries += l->out.tries;
    if (l->out.ack) {
        struct link_neighbor *n = neighbor(l, l->out.dst);
        n->frames++;
        n->tries += l->out.tries;
        if (result == LINK_ACKED) {
            n->acked++;
            n->etx = mean(n->etx, l->out.tries * LINK_ETX_ONE, n->etx == 0);
        } else if (result == LINK_NO_ACK) {
            n->failed++;
            n->etx = mean(n->etx, ETX_NO_ACK * LINK_ETX_ONE, n->etx == 0);
        }
    }
    l->result = result;
    l->done = true;
    l->state = LINK_IDLE;
}

/* A backoff of a random number of periods below 2 to the exponent; a long one is slept through first. */
static void backoff_start(struct link *l)
{
    uint32_t us = (random_next(l) & ((1u << l->out.exponent) - 1u)) * BACKOFF_PERIOD_US;
    l->out.due = now(l) + us;
    l->state = LINK_BACKOFF;
    if (us >= SLEEP_MIN_US) {
        call(l, rv_timer_set(l->timer, l->timer_bit, us - SLEEP_MIN_US));
    }
}

/*
 * The backoffs from the one at hand, once its sleep is over: the rest of each waited for on the counter,
 * then the assessment, and the frame sent if the channel is clear, or another backoff if not,
 * until one is long enough to sleep through or the channel was busy too often.
 */
static void backoff_end(struct link *l)
{
    for (uint32_t i = 0; i <= MAX_CSMA_BACKOFFS && l->state == LINK_BACKOFF; i++) {
        if (before(now(l), l->out.due - SLEEP_MIN_US)) {
            return;
        }
        while (before(now(l), l->out.due)) {
            receive(l);
        }
        if (radio_send_clear(&l->radio, l->out.frame, l->out.length)) {
            l->out.tries++;
            l->state = LINK_SENDING;
            return;
        }
        l->counts.busy++;
        radio_receive_start(&l->radio);
        if (++l->out.backoffs > MAX_CSMA_BACKOFFS) {
            finish(l, LINK_BUSY);
            return;
        }
        l->out.exponent = l->out.exponent < MAX_BE ? l->out.exponent + 1u : MAX_BE;
        backoff_start(l);
    }
}

/* The frame has gone: done if it asked nothing, otherwise its acknowledgement waited for, and it sent again if none. */
static void sent(struct link *l)
{
    if (!l->out.ack) {
        finish(l, LINK_SENT);
        return;
    }
    uint32_t due = now(l) + ACK_WAIT_US;
    while (before(now(l), due)) {
        if (receive(l)) {
            finish(l, LINK_ACKED);
            return;
        }
    }
    if (l->out.tries > MAX_FRAME_RETRIES) {
        finish(l, LINK_NO_ACK);
        return;
    }
    l->out.backoffs = 0;
    l->out.exponent = MIN_BE;
    backoff_start(l);
    backoff_end(l);
}

void link_init(struct link *l, uint32_t base, uint32_t counter, uint32_t irq, uint32_t irq_bit, uint32_t timer,
               uint32_t timer_bit, uint16_t pan, uint16_t addr, uint32_t seed)
{
    memset(l, 0, sizeof(*l));
    l->counter = counter;
    l->irq = irq;
    l->irq_bit = irq_bit;
    l->timer = timer;
    l->timer_bit = timer_bit;
    l->pan = pan;
    l->addr = addr;
    l->random = seed != 0 ? seed : RANDOM_NONZERO;
    l->seq = (uint8_t)random_next(l);
    l->state = LINK_IDLE;
    l->fault = KERR_OK;
    radio_init(&l->radio, base, (uint32_t)(CCA_DBM - RADIO_LEVEL_DBM));
}

void link_stir(struct link *l, uint32_t bits)
{
    l->random ^= bits;
    random_next(l);
}

int32_t link_power(struct link *l, int32_t dbm)
{
    return radio_power(&l->radio, dbm);
}

void link_listen(struct link *l, uint32_t channel)
{
    radio_channel(&l->radio, channel);
    radio_receive_start(&l->radio);
    arm(l);
}

void link_off(struct link *l)
{
    radio_off(&l->radio);
    call(l, rv_irq_set(l->irq, 0));
    call(l, rv_timer_set(l->timer, 0, 0));
    l->state = LINK_IDLE;
}

bool link_send(struct link *l, uint16_t dst, const uint8_t *payload, uint32_t length)
{
    if (l->state != LINK_IDLE || length > LINK_PAYLOAD_MAX) {
        return false;
    }
    struct mac154_data d = {
        .pan = l->pan, .dst = dst, .src = l->addr, .seq = l->seq++,
        .ack_request = dst != LINK_BROADCAST, .payload = payload, .length = length,
    };
    l->out.length = mac154_data_frame(l->out.frame, sizeof(l->out.frame), &d);
    l->out.dst = dst;
    l->out.seq = d.seq;
    l->out.ack = d.ack_request;
    l->out.backoffs = 0;
    l->out.exponent = MIN_BE;
    l->out.tries = 0;
    l->done = false;
    l->counts.frames++;
    backoff_start(l);
    backoff_end(l);
    arm(l);
    return true;
}

void link_wake(struct link *l, uint32_t bits)
{
    if (l->state == LINK_SENDING && radio_sent(&l->radio)) {
        sent(l);
    }
    if (l->state != LINK_SENDING) {
        receive(l);
    }
    if (l->state == LINK_BACKOFF && ((bits & l->timer_bit) || !before(now(l), l->out.due - SLEEP_MIN_US))) {
        backoff_end(l);
    }
    arm(l);
}

bool link_done(struct link *l, enum link_result *result)
{
    if (!l->done) {
        return false;
    }
    l->done = false;
    *result = l->result;
    return true;
}

bool link_take(struct link *l, struct link_frame *f)
{
    if (l->queue_count == 0) {
        return false;
    }
    *f = l->queue[l->queue_head];
    l->queue_head = (l->queue_head + 1u) % LINK_QUEUE;
    l->queue_count--;
    return true;
}
