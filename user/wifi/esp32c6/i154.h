#ifndef RVUOS_WIFI_I154_H
#define RVUOS_WIFI_I154_H

/*
 * i154.c: the ESP32-C6's IEEE 802.15.4 radio, in place of Wi-Fi when the page names a channel for it, i154.
 * It brings the radio up as ESP-IDF's esp_ieee802154_enable does, surveys the energy on every channel,
 * then exchanges frames with the nRF52840 on the page's channel, see user/thread/, until the root task asks it to leave.
 */

#include "drv.h"
#include "lib/self.h"

__attribute__((noreturn)) void i154_run(struct drv *d, struct self *own);

#endif
