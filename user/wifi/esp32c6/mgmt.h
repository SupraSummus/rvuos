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

/*
 * The largest association request: an SSID of 32 bytes, the rates, an RSN and an RSNX element of 255 bytes each,
 * and HT's capabilities and WMM's information.
 */
#define MGMT_ASSOC_MAX (24u + 4u + 2u + 32u + 2u + 8u + 2u + 4u + 2u * (2u + 255u) + 2u + 26u + 2u + 7u)

/*
 * The beacon intervals between two of the station's wakes, as its association tells the access point,
 * ESP-IDF's default: a station that sleeps hears a beacon at least this often, see sta.c's power save.
 */
#define MGMT_LISTEN_INTERVAL 3u

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
 * An SAE Authentication, WPA3's personal (IEEE 802.11 12.4), from sta to the access point ap:
 * its transaction, MGMT_SAE_COMMIT or MGMT_SAE_CONFIRM, its status, and the len bytes of the message at body,
 * which the supplicant makes; MGMT_AUTH_FIXED bytes longer.
 */
#define MGMT_AUTH_FIXED  (24u + 6u)
#define MGMT_SAE_COMMIT  1u
#define MGMT_SAE_CONFIRM 2u
uint32_t mgmt_sae(uint8_t *f, uint32_t size, const uint8_t *sta, const uint8_t *ap, uint16_t transaction,
                  uint16_t status, const uint8_t *body, uint32_t len);

/* What an SAE Authentication read carries: its transaction, its status, and its message, in the frame. */
struct mgmt_sae {
    uint16_t transaction, status;
    const uint8_t *body;
    uint32_t len;
};

/* An SAE Authentication from the access point ap to sta, a commit or a confirm, read into s. */
int mgmt_sae_read(const uint8_t *f, uint32_t len, const uint8_t *sta, const uint8_t *ap, struct mgmt_sae *s);

/*
 * An association request from sta to the access point ap, for ssid, with the rates the station takes,
 * and the RSN and RSNX elements the supplicant wrote, whole, either 0 if none;
 * its capabilities say the network's privacy where an RSN element is given, as the network's beacon says it.
 * asks, MGMT_ASSOC_ bits, says what else the station asks for: QoS, by WMM's information, and HT, its capabilities
 * 20 MHz and MCS 0 to 7, QoS with it, since an access point gives HT to a QoS station alone.
 */
#define MGMT_ASSOC_QOS 0x1u
#define MGMT_ASSOC_HT  0x2u
uint32_t mgmt_assoc_request(uint8_t *f, uint32_t size, const uint8_t *sta, const uint8_t *ap, const char *ssid,
                            const uint8_t *rsn, const uint8_t *rsnx, uint32_t asks);

/*
 * An ADDBA Request from the access point ap to sta, the frame in the clear, its dialog token and its Block Ack
 * parameters read into a; and the Response that declines it, IEEE 802.11's status 37, the parameters echoed and no
 * timeout: the station takes no block acknowledgement, which the MAC would have to send for it.
 */
struct mgmt_addba {
    uint8_t token;
    uint16_t params;
};
int mgmt_addba_read(const uint8_t *f, uint32_t len, const uint8_t *sta, const uint8_t *ap, struct mgmt_addba *a);
uint32_t mgmt_addba_decline(uint8_t *f, uint32_t size, const uint8_t *sta, const uint8_t *ap,
                            const struct mgmt_addba *a);

/*
 * The access point ap's answer to sta's association: its status into *status, and the station's AID into *aid.
 * A refusal to try again later (status 30) may carry the time to come back, in 802.11's time units, into
 * *comeback_tu, or 0 when it names none. *qos is 1 where it took the station for QoS, its WMM Parameter element said.
 */
int mgmt_assoc_answer(const uint8_t *f, uint32_t len, const uint8_t *sta, const uint8_t *ap, uint16_t *status,
                      uint16_t *aid, uint32_t *comeback_tu, int *qos);

/* A deauthentication from sta to the access point ap, for reason, IEEE 802.11's: MGMT_LEAVING as the station leaves. */
#define MGMT_LEAVING 3u
uint32_t mgmt_deauth(uint8_t *f, uint32_t size, const uint8_t *sta, const uint8_t *ap, uint16_t reason);

/* An association refused to try again later, IEEE 802.11's status, whose answer names a time to come back. */
#define MGMT_TRY_AGAIN 30u

/* A deauthentication or a disassociation to sta from the access point ap, its reason into *reason. */
int mgmt_let_go(const uint8_t *f, uint32_t len, const uint8_t *sta, const uint8_t *ap, uint16_t *reason);

/* Whether f is a deauthentication or disassociation from the access point ap, to the station sta or, with
 * *group set, to every station; the reason is not read, which a protected frame's is not in the clear. */
int mgmt_let_go_kind(const uint8_t *f, uint32_t len, const uint8_t *sta, const uint8_t *ap, int *group);

/*
 * What the station does with a deauthentication or disassociation from its access point. Without PMF protecting
 * management frames (active), any of them ends the link. Under it, a unicast one must be protected and verified
 * (its MIC and replay check) and a group one must carry a valid BIP MIC (verified too); anything else is the
 * forgery PMF exists to stop, and is ignored, counted. See TODO.md.
 *
 * The cost of that ignorance is an access point that lost the station's state and now says so unprotected: the
 * station would hang on a link the access point no longer serves. 802.11w's answer is an SA Query the station
 * sends after an unprotected deauthentication, and leaves on no answer; that is (c), see TODO.md.
 */
#define MGMT_LET_GO_ACCEPT 1
#define MGMT_LET_GO_IGNORE 2
int mgmt_let_go_policy(int active, int group, int protected_, int verified);

/*
 * An SA Query request or response, IEEE 802.11w's: an Action frame of category SA Query to the access point ap
 * from the station sta, its transaction id; 28 bytes, robust, and so to be protected under the pairwise key.
 */
uint32_t mgmt_sa_query(uint8_t *f, uint32_t size, const uint8_t *sta, const uint8_t *ap, int response, uint16_t id);

/* An SA Query from ap to sta: *response 1 for an answer, 0 for a request, and its transaction id into *id. */
int mgmt_sa_query_read(const uint8_t *f, uint32_t len, const uint8_t *sta, const uint8_t *ap, int *response,
                       uint16_t *id);

/* Whether f's Frame Control names an Action frame, whose body a reader may take up, before decrypting it. */
int mgmt_is_action(const uint8_t *f, uint32_t len);

/*
 * Whether the group-addressed robust management frame f carries a valid BIP-CMAC-128 MIC (IEEE 802.11 12.5.4)
 * under the IGTK igtk, whose key id is igtk_id and whose last packet number is *ipn: 1, with *ipn advanced to
 * the frame's, or 0 if the frame's MIC element names another key, its IPN is not above *ipn, or its MIC is not
 * the one that data gives. A group deauthentication or disassociation is worth heeding only so.
 */
int mgmt_bip_verify(const uint8_t *f, uint32_t len, const uint8_t *igtk, uint8_t igtk_id, uint64_t *ipn);

/*
 * The SA Query's next move, from the time it started and last sent, 802.11w's retry and maximum timeouts:
 * SA_QUERY_SEND now, SA_QUERY_WAIT, or SA_QUERY_GIVE_UP, the access point having lost the station.
 */
#define SA_QUERY_SEND     0
#define SA_QUERY_WAIT     1
#define SA_QUERY_GIVE_UP  2
#define SA_QUERY_RETRY_US 200000u  /* between two requests, dot11AssociationSAQueryRetryTimeout */
#define SA_QUERY_MAX_US   1000000u /* the whole query, dot11AssociationSAQueryMaximumTimeout */
int mgmt_sa_query_step(uint64_t now_us, uint64_t start_us, uint64_t last_us);

/*
 * A data frame carries an Ethernet type and payload behind RFC 1042's LLC/SNAP header, as IEEE 802.11 carries them.
 * mgmt_data makes one from sta through the access point ap to dest, unprotected, with the len bytes at body,
 * MGMT_DATA_FIXED bytes longer: the frame's header, the LLC/SNAP header and the type;
 * with body 0 the len bytes are left for the caller to fill.
 */
#define MGMT_DATA_FIXED (24u + 8u)
uint32_t mgmt_data(uint8_t *f, uint32_t size, const uint8_t *sta, const uint8_t *ap, const uint8_t *dest,
                   uint16_t proto, const uint8_t *body, uint32_t len);

/*
 * An A-MSDU, several MSDUs in one QoS Data frame (IEEE 802.11 9.3.2.2): mgmt_amsdu_start writes the frame's header
 * from sta through the access point ap, TID 0 and the A-MSDU Present bit set, and returns its length,
 * MGMT_AMSDU_FIXED; each mgmt_amsdu_add then appends a subframe to the frame at, len bytes long, the one before
 * padded to a whole word from the A-MSDU's start: the destination dest, the source src, and the MSDU, the Ethernet
 * type proto and the len bytes at body behind RFC 1042's LLC/SNAP header. It returns the frame's new length, or 0 if
 * the subframe does not fit size. An access point that takes HT takes an A-MSDU of 3839 bytes, MGMT_AMSDU_MAX.
 * With body 0 the len bytes are left for the caller to fill, the last of the frame's new length.
 */
#define MGMT_AMSDU_FIXED (24u + 2u)
#define MGMT_AMSDU_MAX   3839u
uint32_t mgmt_amsdu_start(uint8_t *f, uint32_t size, const uint8_t *sta, const uint8_t *ap);
uint32_t mgmt_amsdu_add(uint8_t *f, uint32_t size, uint32_t at, const uint8_t *dest, const uint8_t *src,
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

/*
 * A Null frame from sta to the access point ap, a data frame with no body, which the access point acknowledges:
 * the station asks by it whether the access point still hears it, and tells it, by its Power Management bit,
 * whether the station sleeps between beacons, sleeps 1, or stays awake, 0.
 */
uint32_t mgmt_null(uint8_t *f, uint32_t size, const uint8_t *sta, const uint8_t *ap, int sleeps);

/*
 * A beacon of the access point ap, by its subtype and addresses alone, its elements not read:
 * cheap enough for the receiving's thread to count every one by.
 */
int mgmt_beacon_of(const uint8_t *f, uint32_t len, const uint8_t *ap);

/*
 * What the access point's beacon tells a station that sleeps between beacons, from its TIM element (IEEE 802.11
 * 9.4.2.5): its clock and the beacons' interval, which say when the next comes; how many beacons until the next
 * DTIM beacon, 0 for this one, and how many from one to the next; whether, this being a DTIM beacon, frames to the
 * group follow it; and whether the access point holds frames for the station of AID aid.
 */
struct mgmt_tim {
    uint64_t tsf;         /* the access point's clock when it sent the beacon, in us */
    uint16_t interval;    /* between beacons, in time units of 1024 us */
    uint8_t dtim_count;
    uint8_t dtim_period;  /* 1 or more */
    uint8_t group;
    uint8_t ours;
};

/* The beacon of the access point ap, its TIM element read into t; one with none, or a DTIM period of 0, is refused. */
int mgmt_tim(const uint8_t *f, uint32_t len, const uint8_t *ap, uint16_t aid, struct mgmt_tim *t);

/* An access point's frame to the group that says the station is let go: its reason into *reason. */
int mgmt_let_go_group(const uint8_t *f, uint32_t len, const uint8_t *ap, uint16_t *reason);

#endif
