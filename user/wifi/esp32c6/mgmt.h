#ifndef RVUOS_WIFI_MGMT_H
#define RVUOS_WIFI_MGMT_H

/*
 * The station's own management frames: what an access point announces, what the station sends,
 * and the answers it reads, on hostap's parser of elements and its definitions of IEEE 802.11's; see TODO.md.
 * They make no system call, and run on the host too, under the sanitizers, test/mgmt-test.c.
 * A frame is the 802.11 frame without its FCS.
 * A builder writes at most size bytes at f and returns the frame's length, or 0 if it does not fit;
 * it leaves the sequence number 0, for mac_tx to give.
 * A reader reads no byte past the len at f, and returns 1 if the frame is the one it reads, 0 otherwise.
 */

#include <stdint.h>

/* The largest frame a builder here makes: a probe request for an SSID of 32 bytes. */
#define MGMT_FRAME_MAX (24u + 2u + 32u + 2u + 8u + 2u + 4u + 2u + 1u)

/* What a beacon or a probe response says of its network. */
struct mgmt_beacon {
    uint8_t bssid[6];
    uint8_t ssid_len;
    uint8_t ssid[32];
    uint8_t channel;        /* its DS parameter set's, 1 to 14, or the one it was heard on without one */
    uint8_t probe_response; /* 1 if it answers a station's probe, 0 if it is a beacon */
    uint16_t interval;      /* between beacons, in time units of 1024 us */
    uint64_t tsf;           /* the access point's clock when it sent the frame, in us */
};

/* A frame of any type to sta from the access point ap, by its receiver's and transmitter's addresses alone. */
int mgmt_to_station(const uint8_t *f, uint32_t len, const uint8_t *sta, const uint8_t *ap);

/* A beacon or a probe response, its elements parsed; one whose DS parameter set names no channel 1 to 14 is refused. */
int mgmt_beacon(const uint8_t *f, uint32_t len, uint8_t heard_on, struct mgmt_beacon *b);

/* A probe request from sta to every station, for ssid, with the rates the station takes and the channel it asks on. */
uint32_t mgmt_probe_request(uint8_t *f, uint32_t size, const uint8_t *sta, const char *ssid, uint8_t channel);

/* A probe response to sta for ssid, read into b; *again 1 if the access point sent it again. */
int mgmt_probe_answer(const uint8_t *f, uint32_t len, const uint8_t *sta, const char *ssid, struct mgmt_beacon *b,
                      int *again);

/* An open-system Authentication from sta to the access point ap, the first of the exchange's two. */
uint32_t mgmt_auth_request(uint8_t *f, uint32_t size, const uint8_t *sta, const uint8_t *ap);

/* The access point ap's answer to sta's open-system Authentication, the exchange's second, its status into *status. */
int mgmt_auth_answer(const uint8_t *f, uint32_t len, const uint8_t *sta, const uint8_t *ap, uint16_t *status);

#endif
