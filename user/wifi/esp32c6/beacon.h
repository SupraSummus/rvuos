#ifndef RVUOS_WIFI_BEACON_H
#define RVUOS_WIFI_BEACON_H

/*
 * What a beacon or a probe response says of its network, read by hostap's parser of elements:
 * the first of the station's own logic, see TODO.md.
 * It reads frames from the air, makes no system call, and runs on the host too, under the sanitizers, test/beacon-test.c.
 */

#include <stdint.h>

struct beacon {
    uint8_t bssid[6];
    uint8_t ssid_len;
    uint8_t ssid[32];
    uint8_t channel; /* its DS parameter set's, or the one it was heard on without one */
};

/* 1 if the len bytes at frame, without the FCS, are a beacon or a probe response whose elements parse, read into b. */
int beacon_read(const uint8_t *frame, uint32_t len, uint8_t heard_on, struct beacon *b);

#endif
