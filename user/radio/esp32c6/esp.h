/*
 * SPDX-FileCopyrightText: Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef RVUOS_RADIO_ESP_H
#define RVUOS_RADIO_ESP_H

/*
 * What the driver takes from ESP-IDF's declarations, as ESP-IDF 4d59230d has them in esp_wifi.h,
 * esp_private/wifi_os_adapter.h and esp_wifi_types_generic.h (Apache-2.0);
 * only what the driver uses, and the layout the MAC writes its received frames in.
 */

#include <stdint.h>

typedef int32_t esp_err_t;
#define ESP_OK   0
#define ESP_FAIL -1

/* The event of WIFI_EVENT the driver waits for. */
#define WIFI_EVENT_STA_START 2

/* wifi_auth_mode_t, as far as the driver names it: what the station joined by. */
#define WIFI_AUTH_WPA2_PSK 3
#define WIFI_AUTH_WPA3_PSK 6

/* The ROM's delay, which counts the core's cycles, as many a microsecond as the ROM was last told. */
void ets_delay_us(uint32_t us);
void ets_update_cpu_frequency(uint32_t ticks_per_us);

#include "radio.h" /* the board's */

/*
 * What the MAC writes before each frame it receives, the 92 bytes of esp_wifi_rxctrl_t
 * of ESP-IDF's esp_wifi_he_types.h for the C6's MAC, version 2, of which these bytes are read;
 * on the ESP32-C5, whose MAC is version 3, RADIO_MAC_C5, the 64 bytes of that version's struct.
 */
#ifdef RADIO_MAC_C5
#define RX_CTRL_SIZE      64
#define RX_CTRL_SIG_LEN   56
#define RX_CTRL_STATE     60
#else
#define RX_CTRL_SIZE      92
#define RX_CTRL_SIG_LEN   84 /* bits 0 to 13: the MPDU's length, its FCS included */
#define RX_CTRL_STATE     88 /* 0 if the frame was received whole */
#endif
#define RX_CTRL_RSSI      0  /* int8_t, dBm */
#define RX_CTRL_RATE      1  /* bits 0 to 4: the rate, as ESP-IDF's esp_wifi_rxctrl_t names the field */
#define RX_CTRL_RXEND     8  /* the reception's state at its end, 0 if it succeeded */

/* The MPDU's length, and whether it was received whole. */
static inline uint32_t rx_ctrl_len(const uint8_t *c)
{
    return ((uint32_t)c[RX_CTRL_SIG_LEN] | (uint32_t)c[RX_CTRL_SIG_LEN + 1] << 8) & 0x3fffu;
}

static inline int rx_ctrl_whole(const uint8_t *c)
{
    return c[RX_CTRL_STATE] == 0 && c[RX_CTRL_RXEND] == 0;
}

#endif
