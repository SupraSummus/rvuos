#ifndef RVUOS_WIFI_BTBB_H
#define RVUOS_WIFI_BTBB_H

/*
 * The ESP32-C6's BT baseband, which Wi-Fi's and IEEE 802.15.4's radios share, at 0x600a2000.
 * The driver takes it from Espressif's libbtbb.a one function at a time, see btbb.c:
 * btbb_init is the libraries' bt_bb_v2_init_cmplx, the driver's own, and the two others are IEEE 802.15.4's driver's,
 * of the archive until the driver's own replace them.
 */

#include <stdint.h>

void btbb_init(int print_version);
const int8_t *bt_bb_get_tx_pwr_table(uint8_t *length);
void ieee802154_txon_delay_set(void);

#endif
