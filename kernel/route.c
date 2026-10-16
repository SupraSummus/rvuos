/*
 * Routes between devices, and the router that stands in where a board has none to lend; see route.h
 * and DESIGN.md, "Routes".
 */

#include "route.h"

#include "object.h"

paddr_t channel_route[ROUTE_CHANNELS + (ROUTE_CHANNELS == 0)];

struct route *channel_binding(uint32_t channel)
{
    if (!route_channel(channel)) {
        return NULL;
    }
    paddr_t r = channel_route[channel - ROUTE_FIRST];
    return r != 0 ? p2v(r) : NULL;
}

void route_init(void)
{
#if ROUTE_CHANNELS > 0
    for (uint32_t i = 0; i < ROUTE_CHANNELS; i++) {
        LOOP_BOUND(ROUTE_CHANNELS);
        channel_route[i] = 0;
    }
#endif
    router_init();
}

/* The word the router takes for one of a Route's ends, which OP_ROUTE_END found the frame to hold. */
uint32_t route_word(const struct route *r, uint32_t end)
{
    uint32_t word = 0;
    router_end(end, block_base(&r->ends[end]), block_size(&r->ends[end]), r->names[end], &word);
    return word;
}

/* Armed and connected in the router are one state, here and in route_drop and route_forget. */
void route_arm(struct route *r, bool on)
{
    if (on) {
        router_connect(r->channel, route_word(r, ROUTE_EVENT), route_word(r, ROUTE_TASK));
    } else {
        router_disconnect(r->channel);
    }
    r->armed = on;
}

void route_drop(struct cap *routed)
{
    struct route *r = p2v(cap_holder(routed));
    if (r->armed) {
        route_arm(r, false);
    }
    r->names[routed - r->ends] = 0;
    *routed = (struct cap){ 0 };
}

void route_forget(struct route *r)
{
    if (r->armed) {
        route_arm(r, false);
    }
    channel_route[r->channel - ROUTE_FIRST] = 0;
}

#if ROUTER_FAKE
/*
 * A router of ROUTE_CHANNELS channels that wires nothing:
 * it takes any word of any frame for an end, an event or a task alike, and keeps what it was told,
 * so that binding, connecting, arming and revoking a Route run under QEMU and the host as on a board,
 * and the self-check reads it back as it reads a board's.
 */
static struct {
    uint32_t event, task;
    bool on;
} fake[ROUTE_CHANNELS];

bool router_end(uint32_t end, uint32_t base, uint32_t size, uint32_t name, uint32_t *word)
{
    (void)end;
    if (name % 4 != 0 || name >= size) {
        return false;
    }
    *word = base + name;
    return true;
}

void router_connect(uint32_t channel, uint32_t event, uint32_t task)
{
    fake[channel - ROUTE_FIRST].event = event;
    fake[channel - ROUTE_FIRST].task = task;
    fake[channel - ROUTE_FIRST].on = true;
}

void router_disconnect(uint32_t channel)
{
    fake[channel - ROUTE_FIRST].on = false;
}

bool router_connected(uint32_t channel, uint32_t *event, uint32_t *task)
{
    *event = fake[channel - ROUTE_FIRST].event;
    *task = fake[channel - ROUTE_FIRST].task;
    return fake[channel - ROUTE_FIRST].on;
}

void router_init(void)
{
    for (uint32_t i = 0; i < ROUTE_CHANNELS; i++) {
        LOOP_BOUND(ROUTE_CHANNELS);
        fake[i].event = fake[i].task = 0;
        fake[i].on = false;
    }
}
#elif ROUTE_CHANNELS == 0
/* No router: no channel is lent, so nothing below is reached but the boot's. */
bool router_end(uint32_t end, uint32_t base, uint32_t size, uint32_t name, uint32_t *word)
{
    (void)end;
    (void)base;
    (void)size;
    (void)name;
    (void)word;
    return false;
}

void router_connect(uint32_t channel, uint32_t event, uint32_t task)
{
    (void)channel;
    (void)event;
    (void)task;
}

void router_disconnect(uint32_t channel)
{
    (void)channel;
}

bool router_connected(uint32_t channel, uint32_t *event, uint32_t *task)
{
    (void)channel;
    *event = *task = 0;
    return false;
}

void router_init(void)
{
}
#endif
