/*
 * The 802.15.4 listener: the radio up on channel=, 11 to 26, 25 by default, promiscuous, every frame and every failed
 * reception told, and the energy on the channel sampled all the while; see i154.c.
 * With dump=, a capture a second through the modem's dump engine besides, as dump.h's keys ask:
 * dumpbt=1, dumplane=14 and dumpsource=0 take the BLE baseband's side, which hears what the 802.15.4 receiver does;
 * hw/dump.md says what it found.
 * It sends nothing, not even an acknowledgement.
 */

#include <stdint.h>

#include "drv.h"
#include "dump.h"
#include "i154.h"
#include "osi.h"
#include "wifi/conf.h"

DRV_PROGRAM("the 802.15.4 listener", DRV_NEED_I154 | DRV_NEED_DUMP);

static struct dump_conf conf;

void drv_radio_off(void)
{
    drv_phy_off();
}

static void capture(void)
{
    dump_take(&conf);
}

void drv_run(struct drv *d, struct self *own)
{
    uint32_t channel = config_number(d->conf, d->conf_len, "channel", 25);
    drv_must("a channel of 11 to 26",
             channel >= I154_CHANNEL_FIRST && channel <= I154_CHANNEL_LAST ? ESP_OK : ESP_FAIL);
    dump_configure(d->conf, d->conf_len, &conf);
    i154_listen_run(d, own, channel, conf.samples != 0 ? capture : 0);
}
