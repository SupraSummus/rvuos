#ifndef RVUOS_RADIO_I154_H
#define RVUOS_RADIO_I154_H

/*
 * i154.c: the ESP32-C6's IEEE 802.15.4 radio, which the programs prog/i154.c and prog/i154listen.c run.
 * It brings the radio up as ESP-IDF's esp_ieee802154_enable does, and then runs one of three ways,
 * each until the root task asks the program to leave, and each needing the 802.15.4 MAC's line, DRV_NEED_I154.
 */

#include <stdint.h>

#include "drv.h"
#include "lib/self.h"

/*
 * The exchange: the energy on every channel surveyed, then frames exchanged with the nRF52840 on channel, 11 to 26,
 * see user/thread/, a ping every half second and stream frames a second besides; it fails if it heard nothing of the
 * nRF52840's or none of its own frames was acknowledged.
 */
__attribute__((noreturn)) void i154_run(struct drv *d, struct self *own, uint32_t channel, uint32_t stream);

/*
 * The listener: the radio up on channel, promiscuous, every frame and every failed reception told;
 * second, unless 0, is called once a second while it listens, as the sampler's BLE side takes its captures then.
 */
__attribute__((noreturn)) void i154_listen_run(struct drv *d, struct self *own, uint32_t channel, void (*second)(void));

/* The 802.15.4 bring-up alone, as a trace window; see i154.c. */
__attribute__((noreturn)) void i154_trace_run(struct drv *d);

/* The channels of 802.15.4's 2.4 GHz band. */
#define I154_CHANNEL_FIRST 11u
#define I154_CHANNEL_LAST  26u

#endif
