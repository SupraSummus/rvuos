#include "lib/chan.h"

#include "lib/child.h"
#include "lib/libc.h"

int chan_ready(const struct chan_end *e)
{
    return (__atomic_load_n(&e->gen, __ATOMIC_ACQUIRE) & 1u) != 0;
}

uint8_t *chan_put_begin(const struct chan_end *e)
{
    return chan_ready(e) ? ring_put_begin(&e->tx) : 0;
}

void chan_put_end(const struct chan_end *e, uint32_t len)
{
    if (ring_put_end(&e->tx, len)) {
        rv_signal(e->peer, NOTIFY_ALL_BITS);
    }
}

const uint8_t *chan_get_begin(const struct chan_end *e, uint32_t *len)
{
    return chan_ready(e) ? ring_get_begin(&e->rx, len) : 0;
}

void chan_get_end(const struct chan_end *e)
{
    if (ring_get_end(&e->rx)) {
        rv_signal(e->peer, NOTIFY_ALL_BITS);
    }
}

int chan_send(const struct chan_end *e, const void *data, uint32_t len)
{
    uint8_t *slot = chan_put_begin(e);
    if (slot == 0 || len > e->tx.slot_size) {
        return -1;
    }
    memcpy(slot, data, len);
    chan_put_end(e, len);
    return 0;
}

int chan_changed(const struct chan_end *e, uint32_t *gen)
{
    *gen = __atomic_load_n(&e->gen, __ATOMIC_ACQUIRE);
    return *gen != e->seen;
}

void chan_seen(struct chan_end *e, uint32_t gen)
{
    __atomic_store_n(&e->seen, gen, __ATOMIC_RELEASE);
    rv_signal(CHILD_PARENT, NOTIFY_ALL_BITS);
}

/* Empties the rings of a frame, through a region of the parent's for the moment. */
static uint32_t rings_init(struct self *s, uint32_t frame, const struct ring *a, const struct ring *b)
{
    uint32_t region;
    PASS(region_install(s, frame, RIGHT_R | RIGHT_W, &region));
    ring_init(a);
    ring_init(b);
    return region_free(s, region);
}

/* Written last, once the rest of the end is, and odd: connected. */
static void open_end(struct chan_end *e)
{
    __atomic_store_n(&e->gen, e->gen | 1u, __ATOMIC_RELEASE);
}

uint32_t chan_new(struct self *s, struct chan *ch, uint32_t size, uint32_t slot_size)
{
    PASS(mem_frame(s, size, &ch->frame));
    ch->a = ring_shape(ch->frame.base, size / 2u, slot_size);
    ch->b = ring_shape(ch->frame.base + size / 2u, size / 2u, slot_size);
    uint32_t status = rings_init(s, ch->frame.made, &ch->a, &ch->b);
    if (status != KERR_OK) {
        const char *what = s->what;
        mem_give(s, &ch->frame);
        s->what = what;
    }
    return status;
}

/* One child's side of a connection: the frame, and the other's inbox carved to the other's bit, which is chosen first. */
static uint32_t side(struct self *s, const struct chan *ch, struct child *c, const struct child *other,
                     struct chan_end *e, const struct chan_end *oe)
{
    uint32_t region;
    PASS(child_map(s, c, ch->frame.made, RIGHT_R | RIGHT_W, &region));
    PASS(child_give_bits(s, c, other->inbox, oe->bit, &e->peer));
    return KERR_OK;
}

uint32_t chan_connect(struct self *s, const struct chan *ch, struct child *a, struct chan_end *ea, struct child *b,
                      struct chan_end *eb)
{
    PASS(child_bit(s, a, &ea->bit));
    PASS(child_bit(s, b, &eb->bit));
    PASS(side(s, ch, a, b, ea, eb));
    PASS(side(s, ch, b, a, eb, ea));
    ea->tx = ch->a;
    ea->rx = ch->b;
    eb->tx = ch->b;
    eb->rx = ch->a;
    open_end(ea);
    open_end(eb);
    TRY(s, "tell a child of a channel", child_tell(a));
    TRY(s, "tell a child of a channel", child_tell(b));
    return KERR_OK;
}

uint32_t chan_free(struct self *s, struct chan *ch)
{
    return mem_give(s, &ch->frame);
}

/* Client i's channel: the server writes the first half's ring and reads the second's, the client the other way. */
static struct ring hub_ring(const struct chan_hub *h, uint32_t i, uint32_t half)
{
    return ring_shape(h->frame.base + i * h->size + half * (h->size / 2u), h->size / 2u, h->slot_size);
}

uint32_t chan_hub_new(struct self *s, struct chan_hub *h, struct child *server, struct chan_end *ends, uint32_t count,
                      uint32_t size, uint32_t slot_size)
{
    uint32_t region;
    if (count == 0 || count > CHAN_HUB_MAX) {
        s->what = "a hub of no more clients than it has room for";
        return KERR_INVALID_ARG;
    }
    memset(h, 0, sizeof(*h));
    h->server = server;
    h->ends = ends;
    h->count = count;
    h->size = size;
    h->slot_size = slot_size;
    PASS(mem_frame(s, count * size, &h->frame));
    PASS(child_map(s, server, h->frame.made, RIGHT_R | RIGHT_W, &region));
    for (uint32_t i = 0; i < count; i++) {
        struct chan_end *e = &ends[i];
        PASS(child_bit(s, server, &e->bit));
        PASS(child_slot(s, server, &e->peer));
        e->tx = hub_ring(h, i, 0);
        e->rx = hub_ring(h, i, 1);
    }
    return KERR_OK;
}

int chan_hub_idle(const struct chan_hub *h, uint32_t i)
{
    const struct chan_end *e = &h->ends[i];
    uint32_t gen = __atomic_load_n(&e->gen, __ATOMIC_RELAXED);
    return !(gen & 1u) && __atomic_load_n(&e->seen, __ATOMIC_ACQUIRE) == gen;
}

uint32_t chan_hub_connect(struct self *s, struct chan_hub *h, uint32_t i, struct child *client, struct chan_end *ce)
{
    struct chan_end *se = &h->ends[i];
    uint32_t region;
    if (i >= h->count || !chan_hub_idle(h, i)) {
        s->what = "an idle end of a hub";
        return KERR_STATE;
    }
    PASS(slot_new(s, &h->slices[i]));
    TRY(s, "a client's channel", rv_frame_carve(h->frame.made, i * h->size, h->size, h->slices[i]));
    PASS(rings_init(s, h->slices[i], &se->tx, &se->rx));
    PASS(child_map(s, client, h->slices[i], RIGHT_R | RIGHT_W, &region));
    PASS(child_bit(s, client, &ce->bit));
    PASS(child_give_bits(s, client, h->server->inbox, se->bit, &ce->peer));
    PASS(child_put_bits(s, h->server, se->peer, client->inbox, ce->bit));
    ce->tx = se->rx;
    ce->rx = se->tx;
    open_end(ce);
    open_end(se);
    TRY(s, "tell a client of its channel", child_tell(client));
    TRY(s, "tell a server of a client", child_tell(h->server));
    return KERR_OK;
}

uint32_t chan_hub_close(struct self *s, struct chan_hub *h, uint32_t i)
{
    struct chan_end *se = &h->ends[i];
    if (i >= h->count || !(se->gen & 1u)) {
        s->what = "a connected end of a hub";
        return KERR_STATE;
    }
    /* The channel out of the client, wherever it was installed, and the carve back. */
    TRY(s, "take a client's channel back", rv_cap_revoke(s->table, h->slices[i]));
    PASS(slot_free(s, h->slices[i]));
    h->slices[i] = 0;
    /* The client's inbox out of the server's table, which a client taken down took with it already. */
    uint32_t status = rv_cap_delete(h->server->table, se->peer);
    if (status != KERR_OK && status != KERR_INVALID_CAP) {
        s->what = "take a client's inbox from its server";
        return status;
    }
    __atomic_store_n(&se->gen, se->gen + 1u, __ATOMIC_RELEASE);
    TRY(s, "tell a server a client is gone", child_tell(h->server));
    return KERR_OK;
}

uint32_t chan_hub_free(struct self *s, struct chan_hub *h)
{
    for (uint32_t i = 0; i < h->count; i++) {
        if (h->slices[i] != 0) {
            PASS(slot_free(s, h->slices[i]));
            h->slices[i] = 0;
        }
    }
    return mem_give(s, &h->frame);
}
