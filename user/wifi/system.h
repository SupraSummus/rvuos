#ifndef RVUOS_WIFI_SYSTEM_H
#define RVUOS_WIFI_SYSTEM_H

/*
 * What the root tasks of the Wi-Fi system share, the Pico 2 W's and the ESP32-C6's, each beside a driver of its own:
 * the network process, the link that joins it to the driver, the clients it serves through its hub,
 * each second's question to every child whether its loop still comes round, and the watchdog fed.
 * The root task builds its driver, says what to do with it, and decides when the run ends;
 * it keeps a struct system, which it hands to every call here.
 * This code lies in the image the children run too, so it keeps no global; see wifi.h.
 */

#include <stdint.h>

#include "lib/chan.h"
#include "lib/child.h"
#include "lib/say.h"
#include "lib/self.h"

/* The network process's clients, each on the end of the hub of its index; see wifi.h. */
enum { CLIENT_ECHO, CLIENT_CLOCK, CLIENT_LOGGER, CLIENTS };
#define CLIENT_BIT(i) (1u << (i))

struct client {
    struct child child;
    uint32_t restarts;
    int down; /* taken down, to be built again once its end is idle */
};

/* The codes a run halts with but 0, the end of a run that did what was asked. */
#define SYSTEM_FAILED  1u /* the driver or the network process failed */
#define SYSTEM_BROKEN  2u /* a call of the root task's failed, or what the loader was to place is missing */
#define SYSTEM_FAULTED 3u /* the driver or the network process faulted */
#define SYSTEM_LATE    4u /* the link was not up in time, or the run ended without one */
#define SYSTEM_SILENT  5u /* the driver or the network process stopped answering */

struct system {
    /* From the root task, before any call but system_must. */
    struct self *self;
    const struct out *out;   /* the root task's text */
    const char *board;       /* what the network process's status line says the system runs on */
    uint32_t clients_wanted; /* the CLIENT_BITs of the clients to build once the network process has an address */
    uint32_t log_base;       /* the kernel's log, BOOT_CAP_LOG, which the logger carries to a host */
    struct child driver;     /* which the root task builds, and asks whether it serves; see system_check */

    /* The rest is this code's. */
    struct child network;
    int net_started;
    struct chan link;
    struct chan_hub hub;
    struct client clients[CLIENTS];
    int clients_built;
};

/* The machine halted with code, whose reason the root task said. */
__attribute__((noreturn)) void system_halt(uint32_t code);

/* A step of the root task's that must succeed: one that fails is said, with the library's step, and halts. */
void system_must(struct system *s, const char *what, uint32_t status);

/*
 * The network process, not started, with the system's MAC address,
 * the link between it and the driver, whose end driver_link lies in the driver's page,
 * the hub its clients will reach it through, and its own inbox to wake itself with.
 * The root task's room must hold the network process's data, and the clients' once it has an address;
 * the hub takes a frame of free memory, CLIENT_CHAN_SIZE for each client up to the last wanted, a power of two of them.
 */
void system_net_build(struct system *s, struct chan_end *driver_link, const uint8_t mac[6]);
void system_net_start(struct system *s);

/*
 * One wake's news of the children but the driver's states, which are the root task's, from the bits it woke with.
 * The network process's address is said and builds the clients;
 * a client that faults or fails is taken down, and built again once the network process let go of it, a few times;
 * the driver's fault, or the network process's fault or failure, is said,
 * and its halt's code returned for the root task to halt with, else 0.
 */
uint32_t system_heard(struct system *s, uint32_t bits);

/*
 * A second's check: each child that serves asked again whether its loop comes round,
 * the driver only if driver_serves, and the watchdog fed, which so halts the machine only if the root task stops.
 * A client that missed the last check is built again;
 * the driver or the network process that missed it is said, and SYSTEM_SILENT returned, else 0.
 */
uint32_t system_check(struct system *s, int driver_serves);

/* The network process's and the clients' counters, and what the root task has unused, ending its summary's line. */
void system_summary(struct system *s);

#endif
