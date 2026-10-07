#ifndef RVUOS_WIFI_MGMT_H
#define RVUOS_WIFI_MGMT_H

/*
 * The station's own frames: what an access point announces, the management frames the station sends
 * and the answers it reads, and the data frames that carry EAPOL before any key is installed,
 * on hostap's parser of elements and its definitions of IEEE 802.11's; see TODO.md.
 * They make no system call, and run on the host too, under the sanitizers, test/mgmt-test.c.
 * A frame is the 802.11 frame without its FCS.
 * A builder writes at most size bytes at f and returns the frame's length, or 0 if it does not fit;
 * it leaves the sequence number 0, for mac_tx to give.
 * A reader reads no byte past the len at f, and returns 1 if the frame is the one it reads, 0 otherwise.
 */

#include <stdint.h>

/* The largest probe request, authentication or deauthentication a builder here makes: a probe request for 32 bytes. */
#define MGMT_FRAME_MAX (24u + 2u + 32u + 2u + 8u + 2u + 4u + 2u + 1u)

/* The largest association request: an SSID of 32 bytes, the rates, and an RSN and an RSNX element of 255 bytes each. */
#define MGMT_ASSOC_MAX (24u + 4u + 2u + 32u + 2u + 8u + 2u + 4u + 2u * (2u + 255u))

/* What a beacon or a probe response says of its network. */
struct mgmt_beacon {
    uint8_t bssid[6];
    uint8_t ssid_len;
    uint8_t ssid[32];
    uint8_t channel;        /* its DS parameter set's, 1 to 14, or the one it was heard on without one */
    uint8_t probe_response; /* 1 if it answers a station's probe, 0 if it is a beacon */
    uint16_t interval;      /* between beacons, in time units of 1024 us */
    uint64_t tsf;           /* the access point's clock when it sent the frame, in us */
    const uint8_t *rsn;     /* its RSN element, whole, in the frame read, or 0 if it has none */
    const uint8_t *rsnx;    /* its RSNX element, so, or 0 */
};

/* A frame of any type to sta from the access point ap, by its receiver's and transmitter's addresses alone. */
int mgmt_to_station(const uint8_t *f, uint32_t len, const uint8_t *sta, const uint8_t *ap);

/* A data frame, or a deauthentication or a disassociation, from the access point ap to a group address. */
int mgmt_to_group(const uint8_t *f, uint32_t len, const uint8_t *ap);

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

/*
 * An association request from sta to the access point ap, for ssid, with the rates the station takes,
 * and the RSN and RSNX elements the supplicant wrote, whole, either 0 if none;
 * its capabilities say the network's privacy where an RSN element is given, as the network's beacon says it.
 */
uint32_t mgmt_assoc_request(uint8_t *f, uint32_t size, const uint8_t *sta, const uint8_t *ap, const char *ssid,
                            const uint8_t *rsn, const uint8_t *rsnx);

/*
 * The access point ap's answer to sta's association: its status into *status, and the station's AID into *aid.
 * A refusal to try again later (status 30) may carry the time to come back, in 802.11's time units, into
 * *comeback_tu, or 0 when it names none.
 */
int mgmt_assoc_answer(const uint8_t *f, uint32_t len, const uint8_t *sta, const uint8_t *ap, uint16_t *status,
                      uint16_t *aid, uint32_t *comeback_tu);

/* A deauthentication from sta to the access point ap, for reason, IEEE 802.11's: MGMT_LEAVING as the station leaves. */
#define MGMT_LEAVING 3u
uint32_t mgmt_deauth(uint8_t *f, uint32_t size, const uint8_t *sta, const uint8_t *ap, uint16_t reason);

/* An association refused to try again later, IEEE 802.11's status, whose answer names a time to come back. */
#define MGMT_TRY_AGAIN 30u

/* A deauthentication or a disassociation to sta from the access point ap, its reason into *reason. */
int mgmt_let_go(const uint8_t *f, uint32_t len, const uint8_t *sta, const uint8_t *ap, uint16_t *reason);

/*
 * A data frame carries an Ethernet type and payload behind RFC 1042's LLC/SNAP header, as IEEE 802.11 carries them.
 * mgmt_data makes one from sta through the access point ap to dest, unprotected, with the len bytes at body,
 * MGMT_DATA_FIXED bytes longer: the frame's header, the LLC/SNAP header and the type.
 */
#define MGMT_DATA_FIXED (24u + 8u)
uint32_t mgmt_data(uint8_t *f, uint32_t size, const uint8_t *sta, const uint8_t *ap, const uint8_t *dest,
                   uint16_t proto, const uint8_t *body, uint32_t len);

/* What a data frame read carries, in the frame: its source, its Ethernet type, and its payload. */
struct mgmt_payload {
    const uint8_t *src;
    uint16_t proto;
    const uint8_t *body;
    uint32_t len;
};

/*
 * A data frame from the access point ap to sta or to the group, a Data or a QoS Data frame of one whole MSDU,
 * unprotected, behind RFC 1042's LLC/SNAP header, read into p; a frame protected, a fragment, an A-MSDU,
 * one with an HT control field, or one under another LLC header is refused.
 */
int mgmt_data_read(const uint8_t *f, uint32_t len, const uint8_t *sta, const uint8_t *ap, struct mgmt_payload *p);

/* An access point's frame to the group that says the station is let go: its reason into *reason. */
int mgmt_let_go_group(const uint8_t *f, uint32_t len, const uint8_t *ap, uint16_t *reason);

#endif
