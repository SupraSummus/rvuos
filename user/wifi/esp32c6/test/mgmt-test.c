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

/* What a reader made of the frame it read last. */
static struct mgmt_beacon b;
static uint16_t status;
static int again;

enum reader { BEACON, PROBE_ANSWER, AUTH_ANSWER };

/* What reader makes of len bytes, in a buffer of their own length; a probe answer is asked for "rvuos". */
static int reads(enum reader r, const uint8_t *f, uint32_t len)
{
    uint8_t *copy = malloc(len ? len : 1);
    memcpy(copy, f, len);
    int ok = r == BEACON         ? mgmt_beacon(copy, len, HEARD_ON, &b)
             : r == PROBE_ANSWER ? mgmt_probe_answer(copy, len, sta, "rvuos", &b, &again)
                                 : mgmt_auth_answer(copy, len, sta, ap, &status);
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

    for (uint32_t size = 0; size <= len; size++) {
        uint8_t *g = malloc(size ? size : 1);
        check(mgmt_probe_request(g, size, sta, "rvuos", 6) == (size == len ? len : 0),
              "a probe request written only into a buffer it fits");
        free(g);
    }
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

int main(void)
{
    beacon();
    probe_answer();
    probe_request();
    auth_request();
    auth_answer();
    printf("mgmt-test: %s\n", failures ? "FAILED" : "ok");
    return failures != 0;
}
