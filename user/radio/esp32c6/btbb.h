#ifndef RVUOS_RADIO_BTBB_H
#define RVUOS_RADIO_BTBB_H

/*
 * The ESP32-C6's BT baseband, which Wi-Fi's and IEEE 802.15.4's radios share, at 0x600a2000.
 * The driver took it from Espressif's libbtbb.a whole, see btbb.c:
 * btbb_init is the libraries' bt_bb_v2_init_cmplx, the driver's own, and the two others are IEEE 802.15.4's driver's,
 * all of them the driver's own now, the archive linked no more.
 */

#include <stdint.h>

void btbb_init(void);
const int8_t *bt_bb_get_tx_pwr_table(uint8_t *length);
void ieee802154_txon_delay_set(void);

#endif
