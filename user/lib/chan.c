#include "lib/chan.h"

#include "lib/child.h"
#include "lib/libc.h"

int chan_ready(const struct chan_end *e)
{
    return __atomic_load_n(&e->ready, __ATOMIC_ACQUIRE) != 0;
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

uint32_t chan_new(struct self *s, struct chan *ch, uint32_t size, uint32_t slot_size)
{
    uint32_t region;
    PASS(mem_frame(s, size, &ch->frame));
    ch->a = ring_shape(ch->frame.base, size / 2u, slot_size);
    ch->b = ring_shape(ch->frame.base + size / 2u, size / 2u, slot_size);
    uint32_t status = region_install(s, ch->frame.made, RIGHT_R | RIGHT_W, &region);
    if (status == KERR_OK) {
        ring_init(&ch->a);
        ring_init(&ch->b);
        status = region_free(s, region);
    }
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
    __atomic_store_n(&ea->ready, 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&eb->ready, 1u, __ATOMIC_RELEASE);
    TRY(s, "tell a child of a channel", child_tell(a));
    TRY(s, "tell a child of a channel", child_tell(b));
    return KERR_OK;
}

uint32_t chan_free(struct self *s, struct chan *ch)
{
    return mem_give(s, &ch->frame);
}
