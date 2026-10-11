/*
 * IEEE 802.15.4's MAC over the nRF52840's radio; see link.h.
 *
 * The backoffs and retries are mac154.h's; a frame waits macAckWaitDuration for its acknowledgement, 54 symbols,
 * a backoff period, the turnaround of 12, the synchronization header's 10 and six of the acknowledgement's octets.
 *
 * A frame heard during a backoff or the wait for an acknowledgement is acknowledged if it asks,
 * so a neighbour that sends meanwhile is answered;
 * a backoff that ends while an acknowledgement is being sent finds the channel busy.
 */

#include "link.h"

#include "lib/libc.h"
#include "rvuos.h"

#define ACK_WAIT_US (MAC154_ACK_WAIT_SYMBOLS * MAC154_SYMBOL_US)
/*
 * An acknowledgement starts aTurnaroundTime, 12 symbols, after the frame it acknowledges, and not before:
 * its sender turns round to receive meanwhile, and the C6 hears none that comes earlier.
 * The radio ramps up to sending in 40 us, so it is told to send that much before.
 */
#define TURNAROUND_US (12u * MAC154_SYMBOL_US)
#define RAMP_US       40u
/* The alarms: the frame being sent's, for its backoff or its acknowledgement, and the acknowledgement's turnaround. */
#define ALARM_OUT 0u
#define ALARM_ACK 1u
/* The assessment finds the channel busy at -75 dBm or more, 10 dB above the sensitivity 802.15.4 asks of a receiver. */
#define CCA_DBM (-75)
/* A frame never acknowledged counts in ETX as twice the transmissions it was given. */
#define ETX_NO_ACK (2u * (MAC154_MAX_FRAME_RETRIES + 1u))
/* A mean moves by an eighth of the way to each new sample. */
#define MEAN_WEIGHT 8u
/* The random numbers' state, should a seed or the TIMER's bits make it zero, where xorshift would stay. */
#define RANDOM_NONZERO 0x9e3779b9u

static uint32_t now(struct link *l)
{
    return alarm_now(&l->alarm);
}

static void call(struct link *l, uint32_t status)
{
    if (status != KERR_OK && l->fault == KERR_OK) {
        l->fault = status;
    }
}

/* A random number for a backoff: xorshift32, stirred with the TIMER's count, whose low bits the wakes' timing moves. */
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

/* The acknowledgement being sent, its turnaround over. */
static void ack_send(struct link *l)
{
    uint8_t ack[MAC154_ACK_LENGTH];
    radio_send_start(&l->radio, ack, mac154_ack_frame(ack, l->ack_seq));
    l->acking = LINK_ACK_ON_AIR;
}

/*
 * The acknowledgement of the frame numbered seq, which ended no later than the TIMER's count ended,
 * sent once its turnaround is over.
 * A frame is noticed a wake after it ends, and the alarm comes a wake late,
 * so the acknowledgement starts two wakes past the turnaround, well within the 54 symbols its sender waits.
 */
static void acknowledge(struct link *l, uint8_t seq, uint32_t ended)
{
    l->ack_seq = seq;
    l->acking = LINK_ACK_TURNAROUND;
    if (!alarm_set(&l->alarm, ALARM_ACK, ended + TURNAROUND_US - RAMP_US)) {
        ack_send(l);
    }
}

/*
 * A frame heard, which ended no later than the TIMER's count ended:
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
        if (l->state == LINK_ACK_WAIT && mac154_acks(f->bytes, f->length, l->out.seq)) {
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
    /* No frame is short enough to end within another's turnaround; one that did would go unacknowledged. */
    if (d.ack_request && d.dst == l->addr && l->acking == LINK_ACK_NONE) {
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

static void finish(struct link *l, enum link_result result)
{
    l->counts.tries += l->out.csma.tries;
    if (l->out.ack) {
        struct link_neighbor *n = neighbor(l, l->out.dst);
        n->frames++;
        n->tries += l->out.csma.tries;
        if (result == LINK_ACKED) {
            n->acked++;
            n->etx = mean(n->etx, l->out.csma.tries * LINK_ETX_ONE, n->etx == 0);
        } else if (result == LINK_NO_ACK) {
            n->failed++;
            n->etx = mean(n->etx, ETX_NO_ACK * LINK_ETX_ONE, n->etx == 0);
        }
    }
    alarm_off(&l->alarm, ALARM_OUT);
    l->result = result;
    l->done = true;
    l->state = LINK_IDLE;
}

/* A frame, if one has been received since the last: the frame being sent done if it acknowledges it. */
static void receive(struct link *l)
{
    struct radio_frame f;
    if (radio_received(&l->radio, &f) && heard(l, &f, now(l))) {
        finish(l, LINK_ACKED);
    }
}

/* The channel was busy: true if another backoff follows, false if the frame was given up, busy too often. */
static bool busy(struct link *l)
{
    l->counts.busy++;
    if (mac154_csma_busy(&l->out.csma)) {
        return true;
    }
    finish(l, LINK_BUSY);
    return false;
}

/*
 * The assessment once a backoff is over: true if the frame went to the radio or was given up,
 * false if another backoff follows.
 * A frame arriving or an acknowledgement being sent makes the channel as busy as an assessment would find it,
 * and the frame arriving is left to arrive.
 */
static bool assess(struct link *l)
{
    if (l->acking == LINK_ACK_NONE && !radio_receiving(&l->radio)) {
        radio_send_clear(&l->radio, l->out.frame, l->out.length);
        l->state = LINK_SENDING;
        return true;
    }
    return !busy(l);
}

/*
 * Backoffs, each a random number of periods below 2 to the exponent, and the assessment after each,
 * until one is to be waited for on the alarm, the frame went to the radio, or the channel was busy too often;
 * only a backoff of no periods is over before its alarm is set.
 */
static void backoff(struct link *l)
{
    l->state = LINK_BACKOFF;
    while (!alarm_set(&l->alarm, ALARM_OUT, now(l) + mac154_backoff_us(&l->out.csma, random_next(l))) && !assess(l)) {
    }
}

/* No acknowledgement came: the frame sent again after a backoff, or given up once it went too often. */
static void unacknowledged(struct link *l)
{
    if (mac154_csma_retry(&l->out.csma)) {
        backoff(l);
    } else {
        finish(l, LINK_NO_ACK);
    }
}

/* The frame has gone: done if it asked nothing, otherwise its acknowledgement waited for. */
static void sent(struct link *l)
{
    l->out.csma.tries++;
    if (!l->out.ack) {
        finish(l, LINK_SENT);
        return;
    }
    l->state = LINK_ACK_WAIT;
    if (!alarm_set(&l->alarm, ALARM_OUT, now(l) + ACK_WAIT_US)) {
        unacknowledged(l);
    }
}

/* Whether the radio is sending, the link's own frame or an acknowledgement, rather than receiving. */
static bool on_air(const struct link *l)
{
    return l->state == LINK_SENDING || l->acking == LINK_ACK_ON_AIR;
}

/* What the radio's line ended of a frame being sent, the link's own or an acknowledgement. */
static void sending(struct link *l)
{
    enum radio_send s = radio_sending(&l->radio);
    if (l->acking == LINK_ACK_ON_AIR) {
        if (s == RADIO_SENT) {
            l->acking = LINK_ACK_NONE;
            l->counts.acks_sent++;
        }
    } else if (s == RADIO_BUSY) {
        radio_receive_start(&l->radio);
        if (busy(l)) {
            backoff(l);
        }
    } else if (s == RADIO_SENT) {
        sent(l);
    }
}

void link_init(struct link *l, uint32_t radio_base, uint32_t timer_base, uint32_t irq, uint32_t irq_bit,
               uint32_t timer, uint32_t timer_bit, uint16_t pan, uint16_t addr, uint32_t seed)
{
    memset(l, 0, sizeof(*l));
    alarm_init(&l->alarm, timer_base);
    l->irq = irq;
    l->irq_bit = irq_bit;
    l->timer = timer;
    l->timer_bit = timer_bit;
    l->pan = pan;
    l->addr = addr;
    l->random = seed != 0 ? seed : RANDOM_NONZERO;
    l->seq = (uint8_t)random_next(l);
    l->state = LINK_IDLE;
    l->acking = LINK_ACK_NONE;
    l->fault = KERR_OK;
    radio_init(&l->radio, radio_base, (uint32_t)(CCA_DBM - RADIO_LEVEL_DBM));
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
    call(l, rv_irq_set(l->irq, l->irq_bit));
    call(l, rv_irq_set(l->timer, l->timer_bit));
}

void link_off(struct link *l)
{
    radio_off(&l->radio);
    for (uint32_t n = 0; n < ALARMS; n++) {
        alarm_off(&l->alarm, n);
    }
    call(l, rv_irq_set(l->irq, 0));
    call(l, rv_irq_set(l->timer, 0));
    l->state = LINK_IDLE;
    l->acking = LINK_ACK_NONE;
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
    mac154_csma_start(&l->out.csma);
    l->done = false;
    l->counts.frames++;
    backoff(l);
    return true;
}

void link_wake(struct link *l, uint32_t bits)
{
    if (on_air(l)) {
        sending(l);
    }
    if (!on_air(l)) {
        receive(l);
    }
    if (alarm_fired(&l->alarm, ALARM_ACK) && l->acking == LINK_ACK_TURNAROUND) {
        ack_send(l);
    }
    if (alarm_fired(&l->alarm, ALARM_OUT)) {
        if (l->state == LINK_BACKOFF && !assess(l)) {
            backoff(l);
        } else if (l->state == LINK_ACK_WAIT) {
            unacknowledged(l);
        }
    }
    if (bits & l->irq_bit) {
        call(l, rv_irq_set(l->irq, l->irq_bit));
    }
    if (bits & l->timer_bit) {
        call(l, rv_irq_set(l->timer, l->timer_bit));
    }
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
