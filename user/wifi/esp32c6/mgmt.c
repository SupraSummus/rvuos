/* The station's own management frames; see mgmt.h. */

#include "utils/includes.h"

#include "utils/common.h"

#include "common/ieee802_11_common.h"
#include "common/ieee802_11_defs.h"

#include "mgmt.h"

/* Where a beacon's or a probe response's elements start, after its clock, its interval and its capabilities. */
#define BEACON_FIXED offsetof(struct ieee80211_mgmt, u.beacon.variable)
/* An authentication's header and its algorithm, transaction and status. */
#define AUTH_FIXED   offsetof(struct ieee80211_mgmt, u.auth.variable)
#define AUTH_FIRST   1
#define AUTH_SECOND  2
#define CHANNEL_LAST 14 /* 2.4 GHz's last */
/* An association request's header, capabilities and listen interval; its answer's capabilities, status and AID. */
#define ASSOC_FIXED  offsetof(struct ieee80211_mgmt, u.assoc_req.variable)
#define ANSWER_FIXED offsetof(struct ieee80211_mgmt, u.assoc_resp.variable)
#define AID_MASK     0x3fffu /* the AID's bits of its field, whose top two are set */
/*
 * The beacon intervals between two of the station's wakes, as it tells the access point, ESP-IDF's default:
 * the station never sleeps yet, so the access point keeps nothing for it.
 */
#define LISTEN_INTERVAL 3u
/* A deauthentication's or a disassociation's header and reason, laid out alike. */
#define REASON_FIXED offsetof(struct ieee80211_mgmt, u.deauth.variable)
/* A QoS Data frame's header is two bytes longer, its QoS control field, whose bit 7 says an A-MSDU follows. */
#define QOS_CONTROL  2u
#define QOS_AMSDU    0x80u

static const uint8_t broadcast[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

/* RFC 1042's LLC/SNAP header, which the Ethernet type follows. */
static const uint8_t rfc1042[6] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00 };

/*
 * The rates the station takes, in units of 500 kb/s: 802.11b's four, which it asks the network to require,
 * then 802.11g's first four, and in the extended element its last four.
 */
static const uint8_t rates[] = { 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24 };
static const uint8_t more_rates[] = { 0x30, 0x48, 0x60, 0x6c };

/* The subtype of a management frame at least fixed bytes long, its header among them, or -1 for any other frame. */
static int subtype(const uint8_t *f, uint32_t len, uint32_t fixed)
{
    const struct ieee80211_mgmt *m = (const struct ieee80211_mgmt *)f;
    if (len < fixed) {
        return -1;
    }
    uint16_t fc = le_to_host16(m->frame_control);
    return (fc & WLAN_FC_PVER) == 0 && WLAN_FC_GET_TYPE(fc) == WLAN_FC_TYPE_MGMT ? WLAN_FC_GET_STYPE(fc) : -1;
}

/* The header of a management frame of subtype stype, from sa to da in the network bssid, its sequence number 0. */
static void header(uint8_t *f, uint16_t stype, const uint8_t *da, const uint8_t *sa, const uint8_t *bssid)
{
    struct ieee80211_mgmt *m = (struct ieee80211_mgmt *)f;
    memset(f, 0, IEEE80211_HDRLEN);
    m->frame_control = IEEE80211_FC(WLAN_FC_TYPE_MGMT, stype);
    memcpy(m->da, da, ETH_ALEN);
    memcpy(m->sa, sa, ETH_ALEN);
    memcpy(m->bssid, bssid, ETH_ALEN);
}

/* An element, its id, its length and its body, at p; the next one's place. */
static uint8_t *element(uint8_t *p, uint8_t id, const uint8_t *body, uint32_t len)
{
    *p++ = id;
    *p++ = (uint8_t)len;
    memcpy(p, body, len);
    return p + len;
}

int mgmt_to_station(const uint8_t *f, uint32_t len, const uint8_t *sta, const uint8_t *ap)
{
    const struct ieee80211_hdr *h = (const struct ieee80211_hdr *)f;
    return len >= offsetof(struct ieee80211_hdr, addr3) && memcmp(h->addr1, sta, ETH_ALEN) == 0 &&
           memcmp(h->addr2, ap, ETH_ALEN) == 0;
}

/* Whether a is a group address, IEEE 802.11's first bit of its first octet. */
static int group_addr(const uint8_t *a)
{
    return (a[0] & 0x01) != 0;
}

int mgmt_to_group(const uint8_t *f, uint32_t len, const uint8_t *ap)
{
    const struct ieee80211_hdr *h = (const struct ieee80211_hdr *)f;
    if (len < offsetof(struct ieee80211_hdr, addr3) || !group_addr(h->addr1) || memcmp(h->addr2, ap, ETH_ALEN) != 0) {
        return 0;
    }
    uint16_t fc = le_to_host16(h->frame_control);
    if (WLAN_FC_GET_TYPE(fc) == WLAN_FC_TYPE_DATA) {
        return 1;
    }
    int stype = WLAN_FC_GET_STYPE(fc);
    return WLAN_FC_GET_TYPE(fc) == WLAN_FC_TYPE_MGMT &&
           (stype == WLAN_FC_STYPE_DEAUTH || stype == WLAN_FC_STYPE_DISASSOC);
}

int mgmt_beacon(const uint8_t *f, uint32_t len, uint8_t heard_on, struct mgmt_beacon *b)
{
    const struct ieee80211_mgmt *m = (const struct ieee80211_mgmt *)f;
    struct ieee802_11_elems elems;
    int stype = subtype(f, len, BEACON_FIXED);
    if ((stype != WLAN_FC_STYPE_BEACON && stype != WLAN_FC_STYPE_PROBE_RESP) ||
        ieee802_11_parse_elems(f + BEACON_FIXED, len - BEACON_FIXED, &elems, 0) == ParseFailed ||
        elems.ssid == NULL || elems.ssid_len > sizeof(b->ssid) ||
        (elems.ds_params && (elems.ds_params[0] < 1 || elems.ds_params[0] > CHANNEL_LAST))) {
        return 0;
    }
    memcpy(b->bssid, m->bssid, ETH_ALEN);
    b->ssid_len = elems.ssid_len;
    memcpy(b->ssid, elems.ssid, elems.ssid_len);
    b->channel = elems.ds_params ? elems.ds_params[0] : heard_on;
    b->probe_response = stype == WLAN_FC_STYPE_PROBE_RESP;
    b->interval = le_to_host16(m->u.beacon.beacon_int);
    b->tsf = WPA_GET_LE64(m->u.beacon.timestamp);
    /* hostap's parser points at an element's body, past its id and length. */
    b->rsn = elems.rsn_ie ? elems.rsn_ie - 2 : NULL;
    b->rsnx = elems.rsnxe ? elems.rsnxe - 2 : NULL;
    return 1;
}

uint32_t mgmt_probe_request(uint8_t *f, uint32_t size, const uint8_t *sta, const char *ssid, uint8_t channel)
{
    uint32_t ssid_len = strlen(ssid);
    uint32_t len = IEEE80211_HDRLEN + 2u + ssid_len + 2u + sizeof(rates) + 2u + sizeof(more_rates) + 2u + 1u;
    if (ssid_len > SSID_MAX_LEN || len > size) {
        return 0;
    }
    header(f, WLAN_FC_STYPE_PROBE_REQ, broadcast, sta, broadcast);
    uint8_t *p = f + IEEE80211_HDRLEN;
    p = element(p, WLAN_EID_SSID, (const uint8_t *)ssid, ssid_len);
    p = element(p, WLAN_EID_SUPP_RATES, rates, sizeof(rates));
    p = element(p, WLAN_EID_EXT_SUPP_RATES, more_rates, sizeof(more_rates));
    element(p, WLAN_EID_DS_PARAMS, &channel, 1);
    return len;
}

int mgmt_probe_answer(const uint8_t *f, uint32_t len, const uint8_t *sta, const char *ssid, struct mgmt_beacon *b,
                      int *again)
{
    const struct ieee80211_mgmt *m = (const struct ieee80211_mgmt *)f;
    uint32_t ssid_len = strlen(ssid);
    /* mgmt_beacon has seen the header whole. */
    if (!mgmt_beacon(f, len, 0, b) || !b->probe_response || memcmp(m->da, sta, ETH_ALEN) != 0 ||
        b->ssid_len != ssid_len || memcmp(b->ssid, ssid, ssid_len) != 0) {
        return 0;
    }
    *again = (le_to_host16(m->frame_control) & WLAN_FC_RETRY) != 0;
    return 1;
}

uint32_t mgmt_auth_request(uint8_t *f, uint32_t size, const uint8_t *sta, const uint8_t *ap)
{
    struct ieee80211_mgmt *m = (struct ieee80211_mgmt *)f;
    if (size < AUTH_FIXED) {
        return 0;
    }
    header(f, WLAN_FC_STYPE_AUTH, ap, sta, ap);
    m->u.auth.auth_alg = host_to_le16(WLAN_AUTH_OPEN);
    m->u.auth.auth_transaction = host_to_le16(AUTH_FIRST);
    m->u.auth.status_code = host_to_le16(WLAN_STATUS_SUCCESS);
    return AUTH_FIXED;
}

int mgmt_auth_answer(const uint8_t *f, uint32_t len, const uint8_t *sta, const uint8_t *ap, uint16_t *status)
{
    const struct ieee80211_mgmt *m = (const struct ieee80211_mgmt *)f;
    if (subtype(f, len, AUTH_FIXED) != WLAN_FC_STYPE_AUTH || memcmp(m->da, sta, ETH_ALEN) != 0 ||
        memcmp(m->sa, ap, ETH_ALEN) != 0 || memcmp(m->bssid, ap, ETH_ALEN) != 0 ||
        le_to_host16(m->u.auth.auth_alg) != WLAN_AUTH_OPEN ||
        le_to_host16(m->u.auth.auth_transaction) != AUTH_SECOND) {
        return 0;
    }
    *status = le_to_host16(m->u.auth.status_code);
    return 1;
}

uint32_t mgmt_assoc_request(uint8_t *f, uint32_t size, const uint8_t *sta, const uint8_t *ap, const char *ssid,
                            const uint8_t *rsn, const uint8_t *rsnx)
{
    struct ieee80211_mgmt *m = (struct ieee80211_mgmt *)f;
    uint32_t ssid_len = strlen(ssid), rsn_len = rsn ? 2u + rsn[1] : 0, rsnx_len = rsnx ? 2u + rsnx[1] : 0;
    uint32_t len = ASSOC_FIXED + 2u + ssid_len + 2u + sizeof(rates) + 2u + sizeof(more_rates) + rsn_len + rsnx_len;
    if (ssid_len > SSID_MAX_LEN || len > size) {
        return 0;
    }
    header(f, WLAN_FC_STYPE_ASSOC_REQ, ap, sta, ap);
    m->u.assoc_req.capab_info = host_to_le16(WLAN_CAPABILITY_ESS | (rsn ? WLAN_CAPABILITY_PRIVACY : 0));
    m->u.assoc_req.listen_interval = host_to_le16(LISTEN_INTERVAL);
    uint8_t *p = f + ASSOC_FIXED;
    p = element(p, WLAN_EID_SSID, (const uint8_t *)ssid, ssid_len);
    p = element(p, WLAN_EID_SUPP_RATES, rates, sizeof(rates));
    p = element(p, WLAN_EID_EXT_SUPP_RATES, more_rates, sizeof(more_rates));
    /* In IEEE 802.11's order: the RSN element early, the RSNX element among the last. */
    if (rsn) {
        p = element(p, rsn[0], rsn + 2, rsn[1]);
    }
    if (rsnx) {
        element(p, rsnx[0], rsnx + 2, rsnx[1]);
    }
    return len;
}

int mgmt_assoc_answer(const uint8_t *f, uint32_t len, const uint8_t *sta, const uint8_t *ap, uint16_t *status,
                      uint16_t *aid)
{
    const struct ieee80211_mgmt *m = (const struct ieee80211_mgmt *)f;
    if (subtype(f, len, ANSWER_FIXED) != WLAN_FC_STYPE_ASSOC_RESP || memcmp(m->da, sta, ETH_ALEN) != 0 ||
        memcmp(m->sa, ap, ETH_ALEN) != 0 || memcmp(m->bssid, ap, ETH_ALEN) != 0) {
        return 0;
    }
    *status = le_to_host16(m->u.assoc_resp.status_code);
    *aid = le_to_host16(m->u.assoc_resp.aid) & AID_MASK;
    return 1;
}

uint32_t mgmt_deauth(uint8_t *f, uint32_t size, const uint8_t *sta, const uint8_t *ap, uint16_t reason)
{
    struct ieee80211_mgmt *m = (struct ieee80211_mgmt *)f;
    if (size < REASON_FIXED) {
        return 0;
    }
    header(f, WLAN_FC_STYPE_DEAUTH, ap, sta, ap);
    m->u.deauth.reason_code = host_to_le16(reason);
    return REASON_FIXED;
}

int mgmt_let_go(const uint8_t *f, uint32_t len, const uint8_t *sta, const uint8_t *ap, uint16_t *reason)
{
    const struct ieee80211_mgmt *m = (const struct ieee80211_mgmt *)f;
    int stype = subtype(f, len, REASON_FIXED);
    if ((stype != WLAN_FC_STYPE_DEAUTH && stype != WLAN_FC_STYPE_DISASSOC) || memcmp(m->da, sta, ETH_ALEN) != 0 ||
        memcmp(m->sa, ap, ETH_ALEN) != 0 || memcmp(m->bssid, ap, ETH_ALEN) != 0) {
        return 0;
    }
    *reason = le_to_host16(m->u.deauth.reason_code);
    return 1;
}

int mgmt_let_go_group(const uint8_t *f, uint32_t len, const uint8_t *ap, uint16_t *reason)
{
    const struct ieee80211_mgmt *m = (const struct ieee80211_mgmt *)f;
    int stype = subtype(f, len, REASON_FIXED);
    if ((stype != WLAN_FC_STYPE_DEAUTH && stype != WLAN_FC_STYPE_DISASSOC) || !group_addr(m->da) ||
        memcmp(m->sa, ap, ETH_ALEN) != 0 || memcmp(m->bssid, ap, ETH_ALEN) != 0) {
        return 0;
    }
    *reason = le_to_host16(m->u.deauth.reason_code);
    return 1;
}

uint32_t mgmt_data(uint8_t *f, uint32_t size, const uint8_t *sta, const uint8_t *ap, const uint8_t *dest,
                   uint16_t proto, const uint8_t *body, uint32_t len)
{
    struct ieee80211_hdr *h = (struct ieee80211_hdr *)f;
    if (size < MGMT_DATA_FIXED || len > size - MGMT_DATA_FIXED) {
        return 0;
    }
    memset(f, 0, IEEE80211_HDRLEN);
    h->frame_control = IEEE80211_FC(WLAN_FC_TYPE_DATA, WLAN_FC_STYPE_DATA) | host_to_le16(WLAN_FC_TODS);
    memcpy(h->addr1, ap, ETH_ALEN);
    memcpy(h->addr2, sta, ETH_ALEN);
    memcpy(h->addr3, dest, ETH_ALEN);
    memcpy(f + IEEE80211_HDRLEN, rfc1042, sizeof(rfc1042));
    WPA_PUT_BE16(f + IEEE80211_HDRLEN + sizeof(rfc1042), proto);
    if (len) {
        memcpy(f + MGMT_DATA_FIXED, body, len);
    }
    return MGMT_DATA_FIXED + len;
}

int mgmt_data_read(const uint8_t *f, uint32_t len, const uint8_t *sta, const uint8_t *ap, struct mgmt_payload *p)
{
    const struct ieee80211_hdr *h = (const struct ieee80211_hdr *)f;
    if (len < IEEE80211_HDRLEN) {
        return 0;
    }
    uint16_t fc = le_to_host16(h->frame_control);
    int stype = WLAN_FC_GET_STYPE(fc);
    uint32_t hdr = IEEE80211_HDRLEN + (stype == WLAN_FC_STYPE_QOS_DATA ? QOS_CONTROL : 0u);
    int to_us = memcmp(h->addr1, sta, ETH_ALEN) == 0 || group_addr(h->addr1);
    /* The QoS control field is read only once the length holds it. */
    if ((fc & WLAN_FC_PVER) != 0 || WLAN_FC_GET_TYPE(fc) != WLAN_FC_TYPE_DATA ||
        (stype != WLAN_FC_STYPE_DATA && stype != WLAN_FC_STYPE_QOS_DATA) ||
        (fc & (WLAN_FC_TODS | WLAN_FC_FROMDS | WLAN_FC_MOREFRAG | WLAN_FC_PROTECTED | WLAN_FC_HTC)) != WLAN_FC_FROMDS ||
        WLAN_GET_SEQ_FRAG(le_to_host16(h->seq_ctrl)) != 0 || len < hdr + sizeof(rfc1042) + 2u ||
        (stype == WLAN_FC_STYPE_QOS_DATA && (f[IEEE80211_HDRLEN] & QOS_AMSDU)) || !to_us ||
        memcmp(h->addr2, ap, ETH_ALEN) != 0 || memcmp(f + hdr, rfc1042, sizeof(rfc1042)) != 0) {
        return 0;
    }
    p->src = h->addr3;
    p->proto = WPA_GET_BE16(f + hdr + sizeof(rfc1042));
    p->body = f + hdr + sizeof(rfc1042) + 2u;
    p->len = len - hdr - sizeof(rfc1042) - 2u;
    return 1;
}
