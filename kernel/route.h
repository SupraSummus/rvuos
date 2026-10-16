#ifndef RVUOS_ROUTE_H
#define RVUOS_ROUTE_H

/*
 * Routes: an event of one device wired to a task of another through a channel of the board's router,
 * with no processor in between; see DESIGN.md, "Routes".
 *
 * A board with a router says in board.h which of its channels the kernel lends, ROUTE_CHANNELS from ROUTE_FIRST,
 * and gives the router_ functions below; with ROUTER_FAKE route.c gives a router of its own, which connects nothing,
 * as QEMU's boards and the host have, so that the operations run there as on silicon.
 * A board with neither lends no channel.
 */

#include <stdbool.h>
#include <stdint.h>

#include "kernel.h"
#include "paddr.h"

#ifndef ROUTE_CHANNELS
#define ROUTE_FIRST    0u
#define ROUTE_CHANNELS 0u
#endif
#ifndef ROUTER_FAKE
#define ROUTER_FAKE 0
#endif

struct cap;
struct route;

/*
 * The board's router.
 * router_end: the word the router takes for an end, ROUTE_EVENT or ROUTE_TASK, named name within a frame,
 * false if the frame [base, base + size) holds no such end.
 * router_connect: channel triggers task on event from now on.
 * router_disconnect: channel triggers nothing.
 * router_connected: whether channel triggers anything, and from what to what, as the router itself says,
 * for the self-check to read back.
 * router_init: every channel the kernel lends triggers nothing, at boot.
 */
bool router_end(uint32_t end, uint32_t base, uint32_t size, uint32_t name, uint32_t *word);
void router_connect(uint32_t channel, uint32_t event, uint32_t task);
void router_disconnect(uint32_t channel);
bool router_connected(uint32_t channel, uint32_t *event, uint32_t *task);
void router_init(void);

/* The Route bound to each channel the kernel lends, from ROUTE_FIRST, 0 for none; at least one entry. */
extern paddr_t channel_route[];

/* Whether the kernel lends the channel. */
static inline bool route_channel(uint32_t channel)
{
    return channel - ROUTE_FIRST < ROUTE_CHANNELS;
}

/* The Route bound to a channel the kernel lends, or NULL. */
struct route *channel_binding(uint32_t channel);

/* Every channel free, and the router's off, at boot. */
void route_init(void);

/* The word the router takes for a Route's end, which is connected. */
uint32_t route_word(const struct route *r, uint32_t end);

/* Arm or disarm a Route whose ends are both connected; the router's channel follows. */
void route_arm(struct route *r, bool on);

/* Clear a Route's end the tree has already let go of: the Route is disarmed first. */
void route_drop(struct cap *routed);

/* A Route its pool's destroy takes: disarmed, and its channel free again. */
void route_forget(struct route *r);

#endif
