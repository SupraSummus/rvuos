/*
 * IEEE 802.15.4 with the nRF52840: the energy on every channel surveyed, then frames exchanged with user/thread/
 * on channel=, 11 to 26, 25 by default, a ping every half second and stream= frames a second besides, up to 200;
 * see i154.c. The run fails if it heard nothing of the nRF52840's or none of its own frames was acknowledged.
 * With trace=, the radio's bring-up alone, as a trace window; see the station's CLAUDE.md, "The trace".
 * tools/thread-pair.sh runs it with the nRF52840.
 */

#include <stdint.h>

#include "drv.h"
#include "i154.h"
#include "osi.h"
#include "wifi/conf.h"

DRV_PROGRAM("the 802.15.4 exchange", DRV_NEED_RNG | DRV_NEED_I154);

void drv_radio_off(void)
{
    drv_phy_off();
}

void drv_run(struct drv *d, struct self *own)
{
    if (d->trace) {
        i154_trace_run(d);
    }
    uint32_t channel = config_number(d->conf, d->conf_len, "channel", 25);
    uint32_t stream = config_number(d->conf, d->conf_len, "stream", 0);
    drv_must("a channel of 11 to 26",
             channel >= I154_CHANNEL_FIRST && channel <= I154_CHANNEL_LAST ? ESP_OK : ESP_FAIL);
    i154_run(d, own, channel, stream <= 200u ? stream : 200u);
}
