/*
 * mgmt.c on the host, under the sanitizers.
 * What the station sends is read back by hostap's parser of elements, and refused a buffer too small;
 * what it reads is read, refused when a field that names it differs, and read or refused at every cut.
 * Each frame lies in a buffer of exactly its length, so that a byte read or written past it fails the run.
 */

#include "utils/includes.h"

#include "utils/common.h"

#include "common/ieee802_11_common.h"
#include "common/ieee802_11_defs.h"

#include "mgmt.h"

#define FC(stype) (WLAN_FC_TYPE_MGMT << 2 | (stype) << 4)
#define HEARD_ON  11 /* the channel a beacon is heard on */
#define NONE      UINT32_MAX /* no cut reads as whole */

static const uint8_t sta[ETH_ALEN] = { 0x02, 0, 0, 0, 0, 0x0a };
static const uint8_t ap[ETH_ALEN] = { 0x02, 0, 0, 0, 0, 0x01 };
static const uint8_t other[ETH_ALEN] = { 0x02, 0, 0, 0, 0, 0x0b };
static const uint8_t broadcast[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static int failures;

static void check(int ok, const char *what)
{
    if (!ok) {
        printf("mgmt-test: FAIL %s\n", what);
        failures++;
    }
}

/*
 * What a reader made of the frame it read last; where a pointer it gave pointed, as an offset into the frame,
 * or -1 for none, since the frame's copy is gone once it returns.
 */
static struct mgmt_beacon b;
static struct mgmt_payload payload;
static uint16_t status, aid, reason;
static uint32_t comeback;
static int again;
static long rsn_at, rsnx_at, src_at, body_at;

enum reader { BEACON, PROBE_ANSWER, AUTH_ANSWER, TO_STATION, ASSOC_ANSWER, LET_GO, DATA };

static long at(const uint8_t *p, const uint8_t *frame)
{
    return p ? (long)(p - frame) : -1;
}

/* What reader makes of len bytes, in a buffer of their own length; a probe answer is asked for "rvuos". */
static int reads(enum reader r, const uint8_t *f, uint32_t len)
{
    uint8_t *copy = malloc(len ? len : 1);
    memcpy(copy, f, len);
    int ok = r == BEACON         ? mgmt_beacon(copy, len, HEARD_ON, &b)
             : r == PROBE_ANSWER ? mgmt_probe_answer(copy, len, sta, "rvuos", &b, &again)
             : r == AUTH_ANSWER  ? mgmt_auth_answer(copy, len, sta, ap, &status)
             : r == ASSOC_ANSWER ? mgmt_assoc_answer(copy, len, sta, ap, &status, &aid, &comeback)
             : r == LET_GO       ? mgmt_let_go(copy, len, sta, ap, &reason)
             : r == DATA         ? mgmt_data_read(copy, len, sta, ap, &payload)
                                 : mgmt_to_station(copy, len, sta, ap);
    if (ok && (r == BEACON || r == PROBE_ANSWER)) {
        rsn_at = at(b.rsn, copy);
        rsnx_at = at(b.rsnx, copy);
    }
    if (ok && r == DATA) {
        src_at = at(payload.src, copy);
        body_at = at(payload.body, copy);
    }
    free(copy);
    return ok;
}

/* Every cut of the len bytes at f refused, but at whole, where the frame reads as one still. */
static void cuts(enum reader r, const uint8_t *f, uint32_t len, uint32_t whole, uint32_t whole_too, const char *what)
{
    for (uint32_t cut = 0; cut < len; cut++) {
        if (reads(r, f, cut) != (cut == whole || cut == whole_too)) {
            printf("mgmt-test: FAIL %s cut at %u bytes\n", what, cut);
            failures++;
        }
    }
}

/* A builder's call, into a buffer g of every size up to len, writes the frame into the one of len alone. */
#define FITS_ONLY(len, what, call)                                \
    for (uint32_t size = 0; size <= (len); size++) {              \
        uint8_t *g = malloc(size ? size : 1);                     \
        check((call) == (size == (len) ? (len) : 0), what);       \
        free(g);                                                  \
    }

static uint32_t header(uint8_t *f, uint16_t fc, const uint8_t *da, const uint8_t *sa, const uint8_t *bssid)
{
    memset(f, 0, 24);
    WPA_PUT_LE16(f, fc);
    memcpy(f + 4, da, ETH_ALEN);
    memcpy(f + 10, sa, ETH_ALEN);
    memcpy(f + 16, bssid, ETH_ALEN);
    return 24;
}

/*
 * A beacon or a probe response from ap: its clock, 100 TU between beacons, its capabilities,
 * then its SSID unless 0, its rates, and its channel. ANNOUNCED is where its elements start.
 */
#define ANNOUNCED 36u
#define RATES     6u
static uint32_t announcement(uint8_t *f, uint16_t stype, const uint8_t *da, const char *ssid, uint8_t channel)
{
    static const uint8_t fixed[] = { 1, 0, 0, 0, 0, 0, 0, 0x80, 100, 0, 0x11, 0x04 };
    static const uint8_t rates[RATES] = { WLAN_EID_SUPP_RATES, 4, 0x82, 0x84, 0x8b, 0x96 };
    uint32_t n = header(f, FC(stype), da, ap, ap);
    memcpy(f + n, fixed, sizeof(fixed));
    n += sizeof(fixed);
    if (ssid) {
        f[n++] = WLAN_EID_SSID;
        f[n++] = (uint8_t)strlen(ssid);
        memcpy(f + n, ssid, strlen(ssid));
        n += strlen(ssid);
    }
    memcpy(f + n, rates, RATES);
    n += RATES;
    f[n++] = WLAN_EID_DS_PARAMS;
    f[n++] = 1;
    f[n++] = channel;
    return n;
}

static void beacon(void)
{
    uint8_t f[128];
    uint32_t len = announcement(f, WLAN_FC_STYPE_BEACON, broadcast, "rvuos", 6);
    check(reads(BEACON, f, len) && b.ssid_len == 5 && memcmp(b.ssid, "rvuos", 5) == 0 &&
              memcmp(b.bssid, ap, ETH_ALEN) == 0 && b.channel == 6 && !b.probe_response,
          "a beacon read");
    check(b.interval == 100 && b.tsf == 0x8000000000000001ull, "a beacon's interval and clock");
    check(reads(BEACON, f, len - 3) && b.channel == HEARD_ON, "a beacon without a channel, on the one it was heard on");
    cuts(BEACON, f, len, ANNOUNCED + 7, ANNOUNCED + 7 + RATES, "a beacon");

    len = announcement(f, WLAN_FC_STYPE_PROBE_RESP, sta, "rvuos", 6);
    check(reads(BEACON, f, len) && b.probe_response, "a probe response read");
    len = announcement(f, WLAN_FC_STYPE_PROBE_REQ, broadcast, "rvuos", 6);
    check(!reads(BEACON, f, len), "a probe request refused");

    len = announcement(f, WLAN_FC_STYPE_BEACON, broadcast, "rvuos", 14);
    check(reads(BEACON, f, len) && b.channel == 14, "a beacon on channel 14 read");
    len = announcement(f, WLAN_FC_STYPE_BEACON, broadcast, "rvuos", 0);
    check(!reads(BEACON, f, len), "a beacon on channel 0 refused");
    len = announcement(f, WLAN_FC_STYPE_BEACON, broadcast, "rvuos", 15);
    check(!reads(BEACON, f, len), "a beacon on channel 15 refused");
    len = announcement(f, WLAN_FC_STYPE_BEACON, broadcast, "0123456789abcdef0123456789abcdefX", 6);
    check(!reads(BEACON, f, len), "an SSID of 33 bytes refused");
    len = announcement(f, WLAN_FC_STYPE_BEACON, broadcast, 0, 6);
    check(!reads(BEACON, f, len), "a beacon without an SSID refused");
}

/* An RSN element of WPA2's personal, CCMP for both, and an RSNX element saying SAE's hash to element. */
static const uint8_t rsn[] = { WLAN_EID_RSN, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4,
                               1, 0, 0x00, 0x0f, 0xac, 2, 0x80, 0 };
static const uint8_t rsnx[] = { WLAN_EID_RSNX, 1, 0x20 };

/* A beacon's RSN and RSNX elements are found whole, where they lie, and none where there are none. */
static void beacon_security(void)
{
    uint8_t f[128];
    uint32_t len = announcement(f, WLAN_FC_STYPE_BEACON, broadcast, "rvuos", 6);
    check(reads(BEACON, f, len) && rsn_at == -1 && rsnx_at == -1, "a beacon without an RSN element, read so");
    memcpy(f + len, rsn, sizeof(rsn));
    memcpy(f + len + sizeof(rsn), rsnx, sizeof(rsnx));
    check(reads(BEACON, f, len + sizeof(rsn) + sizeof(rsnx)) && rsn_at == (long)len &&
              rsnx_at == (long)(len + sizeof(rsn)),
          "a beacon's RSN and RSNX elements found");
    check(reads(BEACON, f, len + sizeof(rsn)) && rsn_at == (long)len && rsnx_at == -1,
          "a beacon's RSN element without an RSNX element");
    check(!reads(BEACON, f, len + sizeof(rsn) - 1), "a beacon whose RSN element is cut refused");
}

static void probe_answer(void)
{
    uint8_t f[128];
    uint32_t len = announcement(f, WLAN_FC_STYPE_PROBE_RESP, sta, "rvuos", 6);
    check(reads(PROBE_ANSWER, f, len) && !again && memcmp(b.bssid, ap, ETH_ALEN) == 0, "a probe answered, read");
    WPA_PUT_LE16(f, FC(WLAN_FC_STYPE_PROBE_RESP) | WLAN_FC_RETRY);
    check(reads(PROBE_ANSWER, f, len) && again, "an answer sent again, read so");
    cuts(PROBE_ANSWER, f, len, ANNOUNCED + 7, ANNOUNCED + 7 + RATES, "a probe answer");

    len = announcement(f, WLAN_FC_STYPE_PROBE_RESP, other, "rvuos", 6);
    check(!reads(PROBE_ANSWER, f, len), "an answer to another station refused");
    len = announcement(f, WLAN_FC_STYPE_PROBE_RESP, sta, "rvuos2", 6);
    check(!reads(PROBE_ANSWER, f, len), "an answer for a network whose name begins with the one asked for refused");
    len = announcement(f, WLAN_FC_STYPE_BEACON, broadcast, "rvuos", 6);
    check(!reads(PROBE_ANSWER, f, len), "a beacon refused");
}

/* The SSID asked for comes first, and alone: an empty one before it, a wildcard, would ask every network. */
static void probe_request(void)
{
    uint8_t f[MGMT_FRAME_MAX];
    struct ieee802_11_elems elems;
    uint32_t len = mgmt_probe_request(f, sizeof(f), sta, "rvuos", 6);
    uint8_t want[24];
    header(want, FC(WLAN_FC_STYPE_PROBE_REQ), broadcast, sta, broadcast);
    check(len > 24 && memcmp(f, want, 24) == 0, "a probe request's header");
    check(f[24] == WLAN_EID_SSID && f[25] == 5 && memcmp(f + 26, "rvuos", 5) == 0, "a probe request's SSID first");
    check(ieee802_11_parse_elems(f + 24, len - 24, &elems, 1) == ParseOK && elems.supp_rates_len == 8 &&
              elems.ext_supp_rates_len == 4 && elems.ds_params && elems.ds_params[0] == 6,
          "a probe request's rates and channel");

    FITS_ONLY(len, "a probe request written only into a buffer it fits", mgmt_probe_request(g, size, sta, "rvuos", 6));
    check(mgmt_probe_request(f, sizeof(f), sta, "0123456789abcdef0123456789abcdef", 6) == MGMT_FRAME_MAX,
          "a probe request for 32 bytes, the largest");
    check(mgmt_probe_request(f, sizeof(f), sta, "0123456789abcdef0123456789abcdefX", 6) == 0,
          "a probe request for 33 bytes refused");
}

static void auth_request(void)
{
    uint8_t f[MGMT_FRAME_MAX], want[30];
    uint32_t n = header(want, FC(WLAN_FC_STYPE_AUTH), ap, sta, ap);
    WPA_PUT_LE16(want + n, WLAN_AUTH_OPEN);
    WPA_PUT_LE16(want + n + 2, 1);
    WPA_PUT_LE16(want + n + 4, WLAN_STATUS_SUCCESS);
    check(mgmt_auth_request(f, sizeof(f), sta, ap) == sizeof(want) && memcmp(f, want, sizeof(want)) == 0,
          "an open-system authentication, the first of two");
}

static uint32_t auth(uint8_t *f, uint16_t stype, const uint8_t *da, const uint8_t *sa, const uint8_t *bssid,
                     uint16_t alg, uint16_t transaction, uint16_t status_code)
{
    uint32_t n = header(f, FC(stype), da, sa, bssid);
    WPA_PUT_LE16(f + n, alg);
    WPA_PUT_LE16(f + n + 2, transaction);
    WPA_PUT_LE16(f + n + 4, status_code);
    return n + 6;
}

static void auth_answer(void)
{
    static const struct {
        uint16_t stype;
        const uint8_t *da, *sa, *bssid;
        uint16_t alg, transaction;
        const char *what;
    } refused[] = {
        { WLAN_FC_STYPE_AUTH, other, ap, ap, WLAN_AUTH_OPEN, 2, "an answer to another station refused" },
        { WLAN_FC_STYPE_AUTH, sta, other, ap, WLAN_AUTH_OPEN, 2, "an answer from another station refused" },
        { WLAN_FC_STYPE_AUTH, sta, ap, other, WLAN_AUTH_OPEN, 2, "an answer in another network refused" },
        { WLAN_FC_STYPE_AUTH, sta, ap, ap, WLAN_AUTH_SAE, 2, "an answer of another algorithm refused" },
        { WLAN_FC_STYPE_AUTH, sta, ap, ap, WLAN_AUTH_OPEN, 1, "a first transaction refused" },
        { WLAN_FC_STYPE_DEAUTH, sta, ap, ap, WLAN_AUTH_OPEN, 2, "a deauthentication refused" },
    };
    uint8_t f[64];
    uint32_t len = auth(f, WLAN_FC_STYPE_AUTH, sta, ap, ap, WLAN_AUTH_OPEN, 2, WLAN_STATUS_UNSPECIFIED_FAILURE);
    check(reads(AUTH_ANSWER, f, len) && status == WLAN_STATUS_UNSPECIFIED_FAILURE, "an answer read, its status");
    cuts(AUTH_ANSWER, f, len, NONE, NONE, "an answer");
    memset(f + len, 0xdd, 8);
    check(reads(AUTH_ANSWER, f, len + 8), "an answer with elements after its fields, read");
    for (uint32_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        len = auth(f, refused[i].stype, refused[i].da, refused[i].sa, refused[i].bssid, refused[i].alg,
                   refused[i].transaction, 0);
        check(!reads(AUTH_ANSWER, f, len), refused[i].what);
    }
}

/* An association request names the network, the rates, then the supplicant's elements, as given, in their order. */
static void assoc_request(void)
{
    uint8_t f[MGMT_ASSOC_MAX], want[28];
    struct ieee802_11_elems elems;
    uint32_t len = mgmt_assoc_request(f, sizeof(f), sta, ap, "rvuos", rsn, rsnx);
    uint32_t n = header(want, FC(WLAN_FC_STYPE_ASSOC_REQ), ap, sta, ap);
    WPA_PUT_LE16(want + n, WLAN_CAPABILITY_ESS | WLAN_CAPABILITY_PRIVACY);
    WPA_PUT_LE16(want + n + 2, 3);
    check(len > sizeof(want) && memcmp(f, want, sizeof(want)) == 0, "an association request's header and fields");
    check(f[28] == WLAN_EID_SSID && f[29] == 5 && memcmp(f + 30, "rvuos", 5) == 0,
          "an association request's SSID first");
    check(ieee802_11_parse_elems(f + 28, len - 28, &elems, 1) == ParseOK && elems.supp_rates_len == 8 &&
              elems.ext_supp_rates_len == 4 && elems.rsn_ie && elems.rsn_ie_len == rsn[1] &&
              memcmp(elems.rsn_ie, rsn + 2, rsn[1]) == 0 && elems.rsnxe && elems.rsnxe_len == rsnx[1] &&
              elems.rsnxe > elems.rsn_ie,
          "an association request's rates, then its RSN and RSNX elements");
    FITS_ONLY(len, "an association request written only into a buffer it fits",
              mgmt_assoc_request(g, size, sta, ap, "rvuos", rsn, rsnx));

    len = mgmt_assoc_request(f, sizeof(f), sta, ap, "rvuos", 0, 0);
    check(len == 28 + 7 + 10 + 6 && WPA_GET_LE16(f + 24) == WLAN_CAPABILITY_ESS,
          "an association request without elements to an open network, its privacy not said");
    len = mgmt_assoc_request(f, sizeof(f), sta, ap, "rvuos", rsn, 0);
    check(ieee802_11_parse_elems(f + 28, len - 28, &elems, 1) == ParseOK && elems.rsn_ie && !elems.rsnxe,
          "an association request with an RSN element alone");

    /* The largest: elements of 255 bytes each, which a builder copies by their own length bytes. */
    uint8_t big_rsn[2 + 255], big_rsnx[2 + 255];
    memset(big_rsn, 0x11, sizeof(big_rsn));
    memset(big_rsnx, 0x22, sizeof(big_rsnx));
    big_rsn[0] = WLAN_EID_RSN;
    big_rsn[1] = 255;
    big_rsnx[0] = WLAN_EID_RSNX;
    big_rsnx[1] = 255;
    check(mgmt_assoc_request(f, sizeof(f), sta, ap, "0123456789abcdef0123456789abcdef", big_rsn, big_rsnx) ==
              MGMT_ASSOC_MAX,
          "an association request for 32 bytes with the largest elements, the largest");
    check(mgmt_assoc_request(f, sizeof(f), sta, ap, "0123456789abcdef0123456789abcdefX", rsn, rsnx) == 0,
          "an association request for 33 bytes refused");
}

static uint32_t assoc_answer_frame(uint8_t *f, uint16_t stype, const uint8_t *da, const uint8_t *sa,
                                   const uint8_t *bssid, uint16_t status_code, uint16_t aid_field)
{
    uint32_t n = header(f, FC(stype), da, sa, bssid);
    WPA_PUT_LE16(f + n, WLAN_CAPABILITY_ESS);
    WPA_PUT_LE16(f + n + 2, status_code);
    WPA_PUT_LE16(f + n + 4, aid_field);
    return n + 6;
}

static void assoc_answer(void)
{
    static const struct {
        uint16_t stype;
        const uint8_t *da, *sa, *bssid;
        const char *what;
    } refused[] = {
        { WLAN_FC_STYPE_ASSOC_RESP, other, ap, ap, "an association answer to another station refused" },
        { WLAN_FC_STYPE_ASSOC_RESP, sta, other, ap, "an association answer from another station refused" },
        { WLAN_FC_STYPE_ASSOC_RESP, sta, ap, other, "an association answer in another network refused" },
        { WLAN_FC_STYPE_REASSOC_RESP, sta, ap, ap, "a reassociation answer refused" },
        { WLAN_FC_STYPE_AUTH, sta, ap, ap, "an authentication refused" },
    };
    uint8_t f[64];
    /* An AID has its top two bits set, which are not its own. */
    uint32_t len = assoc_answer_frame(f, WLAN_FC_STYPE_ASSOC_RESP, sta, ap, ap, WLAN_STATUS_SUCCESS, 0xc000 | 5);
    check(reads(ASSOC_ANSWER, f, len) && status == WLAN_STATUS_SUCCESS && aid == 5, "an association answer, its AID");
    cuts(ASSOC_ANSWER, f, len, NONE, NONE, "an association answer");
    memcpy(f + len, rsn, sizeof(rsn));
    check(reads(ASSOC_ANSWER, f, len + sizeof(rsn)), "an association answer with elements after its fields, read");
    len = assoc_answer_frame(f, WLAN_FC_STYPE_ASSOC_RESP, sta, ap, ap, WLAN_STATUS_AP_UNABLE_TO_HANDLE_NEW_STA, 0);
    check(reads(ASSOC_ANSWER, f, len) && status == WLAN_STATUS_AP_UNABLE_TO_HANDLE_NEW_STA,
          "an association refused, read with its status");
    /* One told to try again later names the time to come back in a Timeout Interval element (EID 56, length 5,
     * type 3, a little-endian value in 802.11's time units); one that names none leaves it 0. */
    len = assoc_answer_frame(f, WLAN_FC_STYPE_ASSOC_RESP, sta, ap, ap, WLAN_STATUS_ASSOC_REJECTED_TEMPORARILY, 0);
    f[len++] = WLAN_EID_TIMEOUT_INTERVAL;
    f[len++] = 5;
    f[len++] = WLAN_TIMEOUT_ASSOC_COMEBACK;
    WPA_PUT_LE32(f + len, 1000);
    len += 4;
    check(reads(ASSOC_ANSWER, f, len) && status == WLAN_STATUS_ASSOC_REJECTED_TEMPORARILY && comeback == 1000,
          "an association to try again later, with its time to come back");
    len = assoc_answer_frame(f, WLAN_FC_STYPE_ASSOC_RESP, sta, ap, ap, WLAN_STATUS_ASSOC_REJECTED_TEMPORARILY, 0);
    check(reads(ASSOC_ANSWER, f, len) && comeback == 0, "and one that names no time");
    /* An element that is a time to come back but one byte long: read as none, and nothing read past it. */
    len = assoc_answer_frame(f, WLAN_FC_STYPE_ASSOC_RESP, sta, ap, ap, WLAN_STATUS_ASSOC_REJECTED_TEMPORARILY, 0);
    f[len++] = WLAN_EID_TIMEOUT_INTERVAL;
    f[len++] = 1;
    f[len++] = WLAN_TIMEOUT_ASSOC_COMEBACK;
    check(reads(ASSOC_ANSWER, f, len) && comeback == 0, "and a shortened time element read as none");
    for (uint32_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        len = assoc_answer_frame(f, refused[i].stype, refused[i].da, refused[i].sa, refused[i].bssid, 0, 1);
        check(!reads(ASSOC_ANSWER, f, len), refused[i].what);
    }
}

static void deauth(void)
{
    uint8_t f[MGMT_FRAME_MAX], want[26];
    uint32_t n = header(want, FC(WLAN_FC_STYPE_DEAUTH), ap, sta, ap);
    WPA_PUT_LE16(want + n, MGMT_LEAVING);
    check(mgmt_deauth(f, sizeof(f), sta, ap, MGMT_LEAVING) == sizeof(want) && memcmp(f, want, sizeof(want)) == 0,
          "a deauthentication as the station leaves");
    FITS_ONLY(sizeof(want), "a deauthentication written only into a buffer it fits",
              mgmt_deauth(g, size, sta, ap, MGMT_LEAVING));
}

static uint32_t reason_frame(uint8_t *f, uint16_t stype, const uint8_t *da, const uint8_t *sa, const uint8_t *bssid,
                             uint16_t reason_code)
{
    uint32_t n = header(f, FC(stype), da, sa, bssid);
    WPA_PUT_LE16(f + n, reason_code);
    return n + 2;
}

static void let_go(void)
{
    static const struct {
        uint16_t stype;
        const uint8_t *da, *sa, *bssid;
        const char *what;
    } refused[] = {
        { WLAN_FC_STYPE_DEAUTH, other, ap, ap, "a deauthentication of another station refused" },
        { WLAN_FC_STYPE_DEAUTH, sta, other, ap, "a deauthentication from another station refused" },
        { WLAN_FC_STYPE_DISASSOC, sta, ap, other, "a disassociation in another network refused" },
        { WLAN_FC_STYPE_AUTH, sta, ap, ap, "an authentication refused" },
    };
    uint8_t f[64];
    uint32_t len = reason_frame(f, WLAN_FC_STYPE_DEAUTH, sta, ap, ap, WLAN_REASON_4WAY_HANDSHAKE_TIMEOUT);
    check(reads(LET_GO, f, len) && reason == WLAN_REASON_4WAY_HANDSHAKE_TIMEOUT, "a deauthentication read, its reason");
    cuts(LET_GO, f, len, NONE, NONE, "a deauthentication");
    len = reason_frame(f, WLAN_FC_STYPE_DISASSOC, sta, ap, ap, WLAN_REASON_DISASSOC_AP_BUSY);
    check(reads(LET_GO, f, len) && reason == WLAN_REASON_DISASSOC_AP_BUSY, "a disassociation read, its reason");
    for (uint32_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        len = reason_frame(f, refused[i].stype, refused[i].da, refused[i].sa, refused[i].bssid, 1);
        check(!reads(LET_GO, f, len), refused[i].what);
    }
    /* The kind is read without the reason, which a protected frame does not carry in the clear. */
    int group = 0;
    len = reason_frame(f, WLAN_FC_STYPE_DEAUTH, sta, ap, ap, 7);
    check(mgmt_let_go_kind(f, len, sta, ap, &group) && !group, "a unicast deauthentication told from a group one");
    len = reason_frame(f, WLAN_FC_STYPE_DEAUTH, broadcast, ap, ap, 7);
    check(mgmt_let_go_kind(f, len, sta, ap, &group) && group, "a deauthentication to every station");
    len = reason_frame(f, WLAN_FC_STYPE_DEAUTH, other, ap, ap, 7);
    check(!mgmt_let_go_kind(f, len, sta, ap, &group), "and one to another station is not the station's to heed");
}

/*
 * The policy under PMF: a unicast deauthentication or disassociation ends the link only protected and verified;
 * an unprotected one, the forgery PMF exists to stop, or a group one, is ignored. Without PMF any of them ends it.
 */
static void let_go_policy(void)
{
    for (int group = 0; group <= 1; group++) {
        for (int prot = 0; prot <= 1; prot++) {
            for (int verified = 0; verified <= 1; verified++) {
                int want = !group && prot && verified ? MGMT_LET_GO_ACCEPT : MGMT_LET_GO_IGNORE;
                check(mgmt_let_go_policy(1, group, prot, verified) == want, "the PMF policy for a leave");
            }
        }
    }
    check(mgmt_let_go_policy(0, 1, 0, 0) == MGMT_LET_GO_ACCEPT, "without PMF even an unprotected group leave ends it");
    check(mgmt_let_go_policy(0, 0, 0, 0) == MGMT_LET_GO_ACCEPT, "and an unprotected unicast one");
}

/* An SA Query the station asks, and the access point's one read back, with the timing decision between them. */
static void sa_query(void)
{
    uint8_t f[64], want[28];
    uint32_t n = header(want, FC(WLAN_FC_STYPE_ACTION), ap, sta, ap);
    want[n++] = WLAN_ACTION_SA_QUERY;
    want[n++] = WLAN_SA_QUERY_REQUEST;
    WPA_PUT_LE16(want + n, 0x1234);
    n += 2;
    check(mgmt_sa_query(f, sizeof(f), sta, ap, 0, 0x1234) == n && memcmp(f, want, n) == 0,
          "an SA Query request to the access point");
    FITS_ONLY(n, "an SA Query written only into a buffer it fits",
              mgmt_sa_query(g, size, sta, ap, 0, 0x1234));

    /* The access point's, read back: to the station, its category, action and transaction id. */
    int response = -1;
    uint16_t id = 0;
    n = header(f, FC(WLAN_FC_STYPE_ACTION), sta, ap, ap);
    f[n++] = WLAN_ACTION_SA_QUERY;
    f[n++] = WLAN_SA_QUERY_REQUEST;
    WPA_PUT_LE16(f + n, 0x1234);
    n += 2;
    check(mgmt_sa_query_read(f, n, sta, ap, &response, &id) && !response && id == 0x1234,
          "an SA Query request from the access point, read");
    f[25] = WLAN_SA_QUERY_RESPONSE;
    check(mgmt_sa_query_read(f, n, sta, ap, &response, &id) && response && id == 0x1234, "and its response");
    f[25] = 7; /* another action */
    check(!mgmt_sa_query_read(f, n, sta, ap, &response, &id), "another action refused");
    f[24] = 7; /* another category */
    check(!mgmt_sa_query_read(f, n, sta, ap, &response, &id), "another category refused");

    /* The Frame Control names an Action before anything is decrypted, and a deauthentication is not one. */
    f[24] = WLAN_ACTION_SA_QUERY;
    f[25] = WLAN_SA_QUERY_REQUEST;
    check(mgmt_is_action(f, n), "an Action told by its Frame Control");
    uint32_t d = reason_frame(f, WLAN_FC_STYPE_DEAUTH, sta, ap, ap, 1);
    check(!mgmt_is_action(f, d), "a deauthentication not taken for one");

    /* The timing at its two boundaries: the retry wait and the whole time, where it gives up. */
    check(mgmt_sa_query_step(SA_QUERY_RETRY_US - 1, 0, 0) == SA_QUERY_WAIT, "waits before the retry time");
    check(mgmt_sa_query_step(SA_QUERY_RETRY_US, 0, 0) == SA_QUERY_SEND, "sends again at it");
    check(mgmt_sa_query_step(SA_QUERY_MAX_US - 1, 0, SA_QUERY_MAX_US - 1) == SA_QUERY_WAIT,
          "waits until the whole time");
    check(mgmt_sa_query_step(SA_QUERY_MAX_US, 0, 0) == SA_QUERY_GIVE_UP, "and gives up at it");
}

/* A data frame to the access point carries the Ethernet type behind RFC 1042's header, then the payload as given. */
static void data(void)
{
    static const uint8_t body[] = { 2, 3, 0, 5, 0xee };
    uint8_t f[64], want[MGMT_DATA_FIXED + sizeof(body)];
    header(want, WLAN_FC_TYPE_DATA << 2 | WLAN_FC_TODS, ap, sta, other);
    memcpy(want + 24, (const uint8_t[]){ 0xaa, 0xaa, 0x03, 0, 0, 0, 0x88, 0x8e }, 8);
    memcpy(want + 32, body, sizeof(body));
    uint32_t len = mgmt_data(f, sizeof(f), sta, ap, other, ETH_P_EAPOL, body, sizeof(body));
    check(len == sizeof(want) && memcmp(f, want, sizeof(want)) == 0, "a data frame to the access point");
    FITS_ONLY(len, "a data frame written only into a buffer it fits",
              mgmt_data(g, size, sta, ap, other, ETH_P_EAPOL, body, sizeof(body)));
    check(mgmt_data(f, sizeof(f), sta, ap, other, ETH_P_EAPOL, 0, 0) == MGMT_DATA_FIXED, "a data frame of no payload");
    check(mgmt_data(f, MGMT_DATA_FIXED, sta, ap, other, ETH_P_EAPOL, body, UINT32_MAX) == 0,
          "a payload longer than any buffer refused");
}

/* A data frame from the access point: fc, the QoS control field if qos, RFC 1042's header or llc, and a payload. */
static uint32_t from_ap(uint8_t *f, uint16_t fc, const uint8_t *da, const uint8_t *transmitter, int qos,
                        uint8_t qos_control, const uint8_t *llc)
{
    static const uint8_t header1042[6] = { 0xaa, 0xaa, 0x03, 0, 0, 0 };
    uint32_t n = header(f, fc, da, transmitter, other);
    if (qos) {
        f[n++] = qos_control;
        f[n++] = 0;
    }
    memcpy(f + n, llc ? llc : header1042, 6);
    n += 6;
    WPA_PUT_BE16(f + n, ETH_P_EAPOL);
    n += 2;
    memcpy(f + n, "\x01\x03\x00\x5f", 4);
    return n + 4;
}

#define DATA_FC(stype, flags) (WLAN_FC_TYPE_DATA << 2 | (stype) << 4 | (flags))

/* Every cut short of the payload's start, at fixed, is refused, and each longer one read, as a shorter payload. */
static void data_cuts(const uint8_t *f, uint32_t len, uint32_t fixed, const char *what)
{
    for (uint32_t cut = 0; cut < len; cut++) {
        if (reads(DATA, f, cut) != (cut >= fixed) || (cut >= fixed && payload.len != cut - fixed)) {
            printf("mgmt-test: FAIL %s cut at %u bytes\n", what, cut);
            failures++;
        }
    }
}

static void data_read(void)
{
    static const uint8_t bridge_tunnel[6] = { 0xaa, 0xaa, 0x03, 0, 0, 0xf8 };
    static const struct {
        uint16_t fc;
        const uint8_t *da, *transmitter;
        int qos;
        uint8_t qos_control;
        uint16_t seq_ctrl;
        const uint8_t *llc;
        const char *what;
    } refused[] = {
        { DATA_FC(WLAN_FC_STYPE_DATA, WLAN_FC_FROMDS), other, ap, 0, 0, 0, 0,
          "a data frame to another station refused" },
        { DATA_FC(WLAN_FC_STYPE_DATA, WLAN_FC_FROMDS), sta, other, 0, 0, 0, 0,
          "a data frame from another station refused" },
        { DATA_FC(WLAN_FC_STYPE_DATA, WLAN_FC_TODS), sta, ap, 0, 0, 0, 0, "a data frame to the DS refused" },
        { DATA_FC(WLAN_FC_STYPE_DATA, 0), sta, ap, 0, 0, 0, 0, "a data frame within no DS refused" },
        { DATA_FC(WLAN_FC_STYPE_DATA, WLAN_FC_FROMDS | WLAN_FC_TODS), sta, ap, 0, 0, 0, 0,
          "a data frame of four addresses refused" },
        { DATA_FC(WLAN_FC_STYPE_DATA, WLAN_FC_FROMDS | WLAN_FC_PROTECTED), sta, ap, 0, 0, 0, 0,
          "a protected data frame refused" },
        { DATA_FC(WLAN_FC_STYPE_DATA, WLAN_FC_FROMDS | WLAN_FC_MOREFRAG), sta, ap, 0, 0, 0, 0,
          "a first fragment refused" },
        { DATA_FC(WLAN_FC_STYPE_DATA, WLAN_FC_FROMDS), sta, ap, 0, 0, 1, 0, "a second fragment refused" },
        { DATA_FC(WLAN_FC_STYPE_QOS_DATA, WLAN_FC_FROMDS | WLAN_FC_HTC), sta, ap, 1, 0, 0, 0,
          "a data frame with an HT control field refused" },
        { DATA_FC(WLAN_FC_STYPE_QOS_DATA, WLAN_FC_FROMDS), sta, ap, 1, 0x80, 0, 0, "an A-MSDU refused" },
        { DATA_FC(WLAN_FC_STYPE_NULLFUNC, WLAN_FC_FROMDS), sta, ap, 0, 0, 0, 0, "a null frame refused" },
        { DATA_FC(WLAN_FC_STYPE_DATA, WLAN_FC_FROMDS), sta, ap, 0, 0, 0, bridge_tunnel,
          "a frame under the bridge tunnel's header refused" },
        { FC(WLAN_FC_STYPE_ACTION) | WLAN_FC_FROMDS, sta, ap, 0, 0, 0, 0, "a management frame refused" },
    };
    uint8_t f[64];
    uint32_t len = from_ap(f, DATA_FC(WLAN_FC_STYPE_DATA, WLAN_FC_FROMDS), sta, ap, 0, 0, 0);
    check(reads(DATA, f, len) && src_at == 16 && payload.proto == ETH_P_EAPOL && body_at == 32 && payload.len == 4,
          "a data frame from the access point read: its source, type and payload");
    data_cuts(f, len, 32, "a data frame");
    len = from_ap(f, DATA_FC(WLAN_FC_STYPE_QOS_DATA, WLAN_FC_FROMDS), sta, ap, 1, 6, 0);
    check(reads(DATA, f, len) && payload.proto == ETH_P_EAPOL && body_at == 34 && payload.len == 4,
          "a QoS data frame read, past its QoS control field");
    data_cuts(f, len, 34, "a QoS data frame");
    for (uint32_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        len = from_ap(f, refused[i].fc, refused[i].da, refused[i].transmitter, refused[i].qos, refused[i].qos_control,
                      refused[i].llc);
        WPA_PUT_LE16(f + 22, refused[i].seq_ctrl);
        check(!reads(DATA, f, len), refused[i].what);
    }

    /* A group-addressed data frame is the reader's too, as the receiving hears it with the group key. */
    len = from_ap(f, DATA_FC(WLAN_FC_STYPE_DATA, WLAN_FC_FROMDS), broadcast, ap, 0, 0, 0);
    check(reads(DATA, f, len) && payload.proto == ETH_P_EAPOL && payload.len == 4, "a data frame to the group read");
    WPA_PUT_LE16(f + 10, 0); /* Address 2 zeroed: not the access point's */
    check(!reads(DATA, f, len), "a group frame from another station refused");
}

/* A frame to the station from its access point is the station's, of any type, once both addresses are whole. */
static void to_station(void)
{
    uint8_t f[64];
    uint32_t len = auth(f, WLAN_FC_STYPE_AUTH, sta, ap, ap, WLAN_AUTH_OPEN, 2, 0);
    for (uint32_t cut = 0; cut <= len; cut++) {
        if (reads(TO_STATION, f, cut) != (cut >= 16)) {
            printf("mgmt-test: FAIL a frame to the station cut at %u bytes\n", cut);
            failures++;
        }
    }
    /* Its third address is another's, the frame's source: the transmitter is the second. */
    len = header(f, WLAN_FC_TYPE_DATA << 2 | WLAN_FC_FROMDS, sta, ap, other);
    check(reads(TO_STATION, f, len), "a data frame from the access point, read");
    len = auth(f, WLAN_FC_STYPE_AUTH, other, ap, ap, WLAN_AUTH_OPEN, 2, 0);
    check(!reads(TO_STATION, f, len), "a frame to another station refused");
    len = auth(f, WLAN_FC_STYPE_AUTH, sta, other, ap, WLAN_AUTH_OPEN, 2, 0);
    check(!reads(TO_STATION, f, len), "a frame from another station in the network refused");

    /* A frame to a group address is the group's when it carries data or a reason, and not when it is a beacon. */
    len = header(f, DATA_FC(WLAN_FC_STYPE_DATA, WLAN_FC_FROMDS), broadcast, ap, other);
    check(mgmt_to_group(f, len, ap), "a group data frame from the access point, read");
    check(!mgmt_to_group(f, len, other), "a group frame from another station refused");
    len = reason_frame(f, WLAN_FC_STYPE_DEAUTH, broadcast, ap, ap, 1);
    check(mgmt_to_group(f, len, ap), "a deauthentication to the group read");
    len = announcement(f, WLAN_FC_STYPE_BEACON, broadcast, "rvuos", 6);
    check(!mgmt_to_group(f, len, ap), "a beacon to the group not taken as the station's");
}

int main(void)
{
    beacon();
    probe_answer();
    probe_request();
    auth_request();
    auth_answer();
    to_station();
    beacon_security();
    assoc_request();
    assoc_answer();
    deauth();
    let_go();
    let_go_policy();
    sa_query();
    data();
    data_read();
    printf("mgmt-test: %s\n", failures ? "FAILED" : "ok");
    return failures != 0;
}
