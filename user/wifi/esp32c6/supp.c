/*
 * The supplicant: hostap's, upstream's own (src/rsn_supp/ of wpa_supplicant, BSD), not ESP-IDF's fork of it,
 * behind the table Espressif's libraries call a supplicant through, struct wpa_funcs of esp.h,
 * which ESP-IDF's esp_wpa_main.c fills for its fork.
 *
 * The libraries pick the access point, authenticate and associate, and encrypt in the MAC.
 * The supplicant gives them the RSN element to associate with, and calls them back to go on;
 * it runs the 4-way and group key handshakes over the EAPOL frames they hand it,
 * and installs the keys the handshakes derive.
 * The libraries call the table on their Wi-Fi task, so the supplicant runs there,
 * its timeouts included; see hostap.c.
 *
 * The libraries send a frame some time after they are handed it, with whichever key is installed then,
 * where hostap's drivers keep the order: hostap installs the pairwise key for sending right after message 4/4,
 * which would then leave encrypted with a key the access point installs only once it has the message.
 * So, as ESP-IDF's fork does, the supplicant holds back what hostap installs after handing over message 4/4,
 * and installs it once the libraries report the message sent.
 */

#include "utils/common.h"

#include "common/defs.h"
#include "common/eapol_common.h"
#include "common/ieee802_11_defs.h"
#include "common/wpa_common.h"
#include "crypto/sha1.h"
#include "rsn_supp/wpa.h"

#include "rsn_supp/pmksa_cache.h" /* after wpa.h, which declares struct wpa_sm */

#include "esp.h"
#include "osi.h"

/* EAPOL's version the supplicant sends, wpa_supplicant's default: version 2 is dropped by some access points. */
#define EAPOL_VERSION_SENT 1

/* What hostap installs after message 4/4: the pairwise key, the group key, the management group key. */
#define HELD_MAX 3

struct held {
    enum wpa_alg alg;
    u8 addr[ETH_ALEN];
    int key_idx, set_tx;
    u8 seq[WPA_KEY_RSC_LEN];
    size_t seq_len;
    u8 key[32];
    size_t key_len;
    enum key_flag key_flag;
};

static struct {
    struct wpa_sm *sm;
    enum wpa_states state;
    u8 own[ETH_ALEN], bssid[ETH_ALEN];
    int m4_out;        /* message 4/4 handed to the libraries, not yet reported sent */
    int complete_held; /* the handshake completed while message 4/4 was out */
    struct held held[HELD_MAX];
    unsigned held_count;

    /* The PMK of the passphrase and SSID last asked, which costs a fifth of a second to derive. */
    u8 pmk[PMK_LEN];
    u8 pmk_ssid[SSID_MAX_LEN];
    size_t pmk_ssid_len;
    char pmk_pass[65];
} supp;

/* --- The crypto suites, as the libraries name them and as hostap does. --- */

static int cipher_to_esp(int cipher)
{
    switch (cipher) {
    case WPA_CIPHER_NONE:
        return WIFI_CIPHER_NONE;
    case WPA_CIPHER_WEP40:
        return WIFI_CIPHER_WEP40;
    case WPA_CIPHER_WEP104:
        return WIFI_CIPHER_WEP104;
    case WPA_CIPHER_TKIP:
        return WIFI_CIPHER_TKIP;
    case WPA_CIPHER_CCMP:
        return WIFI_CIPHER_CCMP;
    case WPA_CIPHER_CCMP | WPA_CIPHER_TKIP:
        return WIFI_CIPHER_TKIP_CCMP;
    case WPA_CIPHER_AES_128_CMAC:
        return WIFI_CIPHER_AES_CMAC128;
    case WPA_CIPHER_SMS4:
        return WIFI_CIPHER_SMS4;
    case WPA_CIPHER_GCMP:
        return WIFI_CIPHER_GCMP;
    case WPA_CIPHER_GCMP_256:
        return WIFI_CIPHER_GCMP256;
    case WPA_CIPHER_BIP_GMAC_128:
        return WIFI_CIPHER_AES_GMAC128;
    case WPA_CIPHER_BIP_GMAC_256:
        return WIFI_CIPHER_AES_GMAC256;
    default:
        return WIFI_CIPHER_UNKNOWN;
    }
}

/* A management group cipher, which the libraries name as wifi_cipher_type_t, as hostap does. */
static int cipher_from_esp(int cipher)
{
    switch (cipher) {
    case WIFI_CIPHER_AES_CMAC128:
        return WPA_CIPHER_AES_128_CMAC;
    case WIFI_CIPHER_AES_GMAC128:
        return WPA_CIPHER_BIP_GMAC_128;
    case WIFI_CIPHER_AES_GMAC256:
        return WPA_CIPHER_BIP_GMAC_256;
    default:
        return WPA_CIPHER_NONE;
    }
}

/*
 * The pairwise or group cipher of the profile, which the libraries name by the number of its bit
 * in ESP-IDF's fork's WPA_CIPHER_ bits, as hostap does: the fork numbers them otherwise.
 */
static int cipher_from_esp_bit(unsigned bit)
{
    switch (bit) {
    case 1:
        return WPA_CIPHER_TKIP;
    case 3:
        return WPA_CIPHER_CCMP;
    case 5:
        return WPA_CIPHER_AES_128_CMAC;
    case 7:
        return WPA_CIPHER_WEP40;
    case 8:
        return WPA_CIPHER_WEP104;
    case 10:
        return WPA_CIPHER_SMS4;
    case 11:
        return WPA_CIPHER_GCMP;
    case 12:
        return WPA_CIPHER_GCMP_256;
    default:
        return WPA_CIPHER_NONE;
    }
}

/* The key management suites ESP-IDF's fork knows, whose bits hostap's share; the libraries read no others. */
#define ESP_KEY_MGMT_KNOWN 0x07c3ffff

static int alg_to_esp(enum wpa_alg alg)
{
    switch (alg) {
    case WPA_ALG_NONE:
        return ESP_ALG_NONE;
    case WPA_ALG_WEP:
        return ESP_ALG_WEP;
    case WPA_ALG_TKIP:
        return ESP_ALG_TKIP;
    case WPA_ALG_CCMP:
        return ESP_ALG_CCMP;
    case WPA_ALG_GCMP:
        return ESP_ALG_GCMP;
    default:
        return -1;
    }
}

/* --- Keys, and those held until message 4/4 is sent. --- */

static int install(const struct held *k)
{
    if (wpa_alg_bip(k->alg)) {
        if (k->alg != WPA_ALG_BIP_CMAC_128 || k->key_len > sizeof(((struct wifi_wpa_igtk *)0)->igtk) ||
            k->seq_len < 6) {
            wpa_printf(MSG_ERROR, "supp: a management group key the libraries do not take: alg %d", k->alg);
            return -1;
        }
        /* The IGTK KDE's layout: the key's index, little-endian, its packet number, the key. */
        struct wifi_wpa_igtk igtk = { { (u8)k->key_idx, (u8)(k->key_idx >> 8) }, { 0 }, { 0 } };
        os_memcpy(igtk.pn, k->seq, 6);
        os_memcpy(igtk.igtk, k->key, k->key_len);
        int r = esp_wifi_set_igtk_internal(WIFI_IF_STA, &igtk);
        forced_memzero(&igtk, sizeof(igtk));
        return r < 0 ? -1 : 0;
    }
    int alg = alg_to_esp(k->alg);
    if (alg < 0) {
        wpa_printf(MSG_ERROR, "supp: a key the libraries do not take: alg %d", k->alg);
        return -1;
    }
    struct held c = *k;
    int r = esp_wifi_set_sta_key_internal(alg, c.addr, c.key_idx, c.set_tx, c.seq, c.seq_len, c.key, c.key_len,
                                          (int)c.key_flag);
    forced_memzero(&c, sizeof(c));
    return r < 0 ? -1 : 0;
}

static void drop_held(void)
{
    forced_memzero(supp.held, sizeof(supp.held));
    supp.held_count = 0;
    supp.m4_out = 0;
    supp.complete_held = 0;
}

static int set_key(void *ctx, int link_id, enum wpa_alg alg, const u8 *addr, int key_idx, int set_tx, const u8 *seq,
                   size_t seq_len, const u8 *key, size_t key_len, enum key_flag key_flag)
{
    (void)ctx;
    (void)link_id;
    /*
     * The pairwise key for receiving alone, before message 4/4, which hostap installs to receive with it at once,
     * and lets fail, as many drivers do not take it: the libraries take the pairwise key once, for both ways,
     * and one installed for receiving first then never encrypts what they send.
     */
    if (key_flag & KEY_FLAG_NEXT) {
        return 0;
    }
    struct held k = { alg, { 0 }, key_idx, set_tx, { 0 }, seq_len, { 0 }, key_len, key_flag };
    if (seq_len > sizeof(k.seq) || key_len > sizeof(k.key)) {
        wpa_printf(MSG_ERROR, "supp: a key longer than any the libraries take");
        return -1;
    }
    /* The libraries file every key of the station's under the access point's address, a group key too. */
    os_memcpy(k.addr, (key_flag & KEY_FLAG_GROUP) || addr == 0 ? supp.bssid : addr, ETH_ALEN);
    if (seq) {
        os_memcpy(k.seq, seq, seq_len);
    }
    if (key) {
        os_memcpy(k.key, key, key_len);
    }
    /*
     * A TKIP group key is the temporal key, then the access point's Michael key for sending, then for receiving.
     * hostap swaps the two Michael keys, so that the one the station checks with lies where Linux's drivers read
     * a key to receive with; the libraries read 802.11's order, as ESP-IDF's fork leaves it, so they go back.
     * Swapped, every frame to the group fails Michael's check, and two in a minute end the association, reason 14.
     */
    if (alg == WPA_ALG_TKIP && (key_flag & KEY_FLAG_GROUP) && key_len == 32) {
        for (unsigned i = 16; i < 24; i++) {
            u8 t = k.key[i];
            k.key[i] = k.key[i + 8];
            k.key[i + 8] = t;
        }
    }
    int r = 0;
    if (!supp.m4_out) {
        r = install(&k);
    } else if (supp.held_count < HELD_MAX) {
        supp.held[supp.held_count++] = k;
    } else {
        wpa_printf(MSG_ERROR, "supp: more keys after message 4/4 than the supplicant holds");
        r = -1;
    }
    forced_memzero(&k, sizeof(k));
    return r;
}

static void set_state(void *ctx, enum wpa_states state)
{
    (void)ctx;
    supp.state = state;
    if (state == WPA_COMPLETED) {
        if (supp.m4_out) {
            supp.complete_held = 1;
        } else {
            esp_wifi_auth_done_internal();
        }
    }
}

/*
 * Whether an EAPOL frame, from its 802.1X header on, is message 4/4 of RSN's 4-way handshake:
 * pairwise, with a MIC, secure, and neither an acknowledgement asked nor a request; message 2/4 is not secure.
 */
static int is_4_of_4(const u8 *buf, size_t len)
{
    const struct ieee802_1x_hdr *hdr = (const struct ieee802_1x_hdr *)buf;
    if (len < sizeof(*hdr) + sizeof(struct wpa_eapol_key) || hdr->type != IEEE802_1X_TYPE_EAPOL_KEY) {
        return 0;
    }
    const struct wpa_eapol_key *key = (const struct wpa_eapol_key *)(hdr + 1);
    u16 info = WPA_GET_BE16(key->key_info);
    return (info & (WPA_KEY_INFO_KEY_TYPE | WPA_KEY_INFO_MIC | WPA_KEY_INFO_SECURE | WPA_KEY_INFO_ACK |
                    WPA_KEY_INFO_REQUEST)) == (WPA_KEY_INFO_KEY_TYPE | WPA_KEY_INFO_MIC | WPA_KEY_INFO_SECURE);
}

/* The libraries' report that an EAPOL frame left: once message 4/4 has, what hostap installed after it. */
static void eapol_sent(uint8_t *buf, size_t len, bool failed)
{
    if (!supp.m4_out || !is_4_of_4(buf, len)) {
        return;
    }
    if (failed) {
        /* The access point sends message 3/4 again, and hostap message 4/4, which leaves held what is held. */
        wpa_printf(MSG_INFO, "supp: message 4/4 was not sent; the keys wait for the next");
        return;
    }
    supp.m4_out = 0;
    for (unsigned i = 0; i < supp.held_count; i++) {
        if (install(&supp.held[i]) < 0) {
            drop_held();
            esp_wifi_deauthenticate_internal(WLAN_REASON_UNSPECIFIED);
            return;
        }
    }
    int complete = supp.complete_held;
    drop_held();
    if (complete) {
        esp_wifi_auth_done_internal();
    }
}

/* --- The rest of what hostap asks of its driver. --- */

static enum wpa_states get_state(void *ctx)
{
    (void)ctx;
    return supp.state;
}

static void deauthenticate(void *ctx, u16 reason)
{
    (void)ctx;
    esp_wifi_deauthenticate_internal((uint8_t)reason);
}

static void reconnect(void *ctx)
{
    (void)ctx;
    esp_wifi_deauthenticate_internal(WLAN_REASON_DEAUTH_LEAVING);
}

static void *get_network_ctx(void *ctx)
{
    return ctx;
}

static int get_bssid(void *ctx, u8 *bssid)
{
    (void)ctx;
    os_memcpy(bssid, supp.bssid, ETH_ALEN);
    return 0;
}

/* The libraries take an Ethernet frame: destination, source, type, and the EAPOL frame. */
static int ether_send(void *ctx, const u8 *dest, u16 proto, const u8 *buf, size_t len)
{
    (void)ctx;
    if (len > 0xffff - 14) {
        return -1;
    }
    u8 *frame = os_malloc(14 + len);
    if (frame == 0) {
        return -1;
    }
    os_memcpy(frame, dest, ETH_ALEN);
    os_memcpy(frame + 6, supp.own, ETH_ALEN);
    WPA_PUT_BE16(frame + 12, proto);
    os_memcpy(frame + 14, buf, len);
    if (proto == ETH_P_EAPOL && is_4_of_4(buf, len)) {
        supp.m4_out = 1;
    }
    int r = esp_wifi_internal_tx(WIFI_IF_STA, frame, (uint16_t)(14 + len));
    os_free(frame);
    return r == 0 ? 0 : -1;
}

/* The access point's RSN and RSNX elements, from the libraries' record of the BSS. */
static void ap_ies(void)
{
    const u8 *rsn = esp_wifi_sta_get_ie(supp.bssid, WLAN_EID_RSN);
    const u8 *rsnx = esp_wifi_sta_get_ie(supp.bssid, WLAN_EID_RSNX);
    wpa_sm_set_ap_rsn_ie(supp.sm, rsn, rsn ? 2u + rsn[1] : 0);
    wpa_sm_set_ap_rsnxe(supp.sm, rsnx, rsnx ? 2u + rsnx[1] : 0);
}

static int get_beacon_ie(void *ctx)
{
    (void)ctx;
    ap_ies();
    return 0;
}

static void cancel_auth_timeout(void *ctx)
{
    (void)ctx;
}

static u8 *alloc_eapol(void *ctx, u8 type, const void *data, u16 data_len, size_t *msg_len, void **data_pos)
{
    (void)ctx;
    struct ieee802_1x_hdr *hdr = os_malloc(sizeof(*hdr) + data_len);
    if (hdr == 0) {
        return 0;
    }
    *msg_len = sizeof(*hdr) + data_len;
    hdr->version = EAPOL_VERSION_SENT;
    hdr->type = type;
    hdr->length = host_to_be16(data_len);
    if (data) {
        os_memcpy(hdr + 1, data, data_len);
    } else {
        os_memset(hdr + 1, 0, data_len);
    }
    if (data_pos) {
        *data_pos = hdr + 1;
    }
    return (u8 *)hdr;
}

/* The libraries keep no PMKSA cache of their own, nor protection hostap could switch on and off. */
static int add_pmkid(void *ctx, void *network_ctx, const u8 *bssid, const u8 *pmkid, const u8 *cache_id, const u8 *pmk,
                     size_t pmk_len, u32 lifetime, u8 reauth_threshold, int akmp)
{
    (void)ctx, (void)network_ctx, (void)bssid, (void)pmkid, (void)cache_id, (void)pmk, (void)pmk_len;
    (void)lifetime, (void)reauth_threshold, (void)akmp;
    return 0;
}

static int remove_pmkid(void *ctx, void *network_ctx, const u8 *bssid, const u8 *pmkid, const u8 *cache_id)
{
    (void)ctx, (void)network_ctx, (void)bssid, (void)pmkid, (void)cache_id;
    return 0;
}

static int mlme_setprotection(void *ctx, const u8 *addr, int protection_type, int key_type)
{
    (void)ctx, (void)addr, (void)protection_type, (void)key_type;
    return 0;
}

/* --- The PMK of a passphrase. --- */

/*
 * The PMK for the passphrase, or the PSK in 64 hexadecimal digits, and the SSID, into supp.pmk:
 * 0, or -1 if pass is neither 8 to 63 characters nor 64 digits.
 */
static int pmk_for(const char *pass, const u8 *ssid, size_t ssid_len)
{
    size_t n = strnlen(pass, 64);
    if (ssid_len > SSID_MAX_LEN || n < 8) {
        return -1;
    }
    if (ssid_len == supp.pmk_ssid_len && os_memcmp(ssid, supp.pmk_ssid, ssid_len) == 0 &&
        os_strncmp(pass, supp.pmk_pass, sizeof(supp.pmk_pass)) == 0) {
        return 0;
    }
    if (n == 64 ? hexstr2bin(pass, supp.pmk, PMK_LEN) : pbkdf2_sha1(pass, ssid, ssid_len, 4096, supp.pmk, PMK_LEN)) {
        supp.pmk_ssid_len = 0;
        return -1;
    }
    os_memcpy(supp.pmk_ssid, ssid, ssid_len);
    supp.pmk_ssid_len = ssid_len;
    os_memcpy(supp.pmk_pass, pass, n);
    supp.pmk_pass[n] = 0;
    return 0;
}

void drv_supp_prepare(const char *ssid, const char *pass)
{
    pmk_for(pass, (const u8 *)ssid, strnlen(ssid, SSID_MAX_LEN));
}

/* --- The table the libraries call. --- */

/* What hostap calls its driver through, which wpa_sm_deinit frees, so a copy in the heap goes to wpa_sm_init. */
static const struct wpa_sm_ctx sm_ctx = {
    .ctx = &supp,
    .set_state = set_state,
    .get_state = get_state,
    .deauthenticate = deauthenticate,
    .reconnect = reconnect,
    .set_key = set_key,
    .get_network_ctx = get_network_ctx,
    .get_bssid = get_bssid,
    .ether_send = ether_send,
    .get_beacon_ie = get_beacon_ie,
    .cancel_auth_timeout = cancel_auth_timeout,
    .alloc_eapol = alloc_eapol,
    .add_pmkid = add_pmkid,
    .remove_pmkid = remove_pmkid,
    .mlme_setprotection = mlme_setprotection,
};

static bool sta_init(void)
{
    if (supp.sm == 0) {
        struct wpa_sm_ctx *ctx = os_memdup(&sm_ctx, sizeof(sm_ctx));
        if (ctx == 0 || (supp.sm = wpa_sm_init(ctx)) == 0) {
            os_free(ctx);
            return false;
        }
    }
    esp_wifi_get_macaddr_internal(WIFI_IF_STA, supp.own);
    wpa_sm_set_own_addr(supp.sm, supp.own);
    supp.state = WPA_DISCONNECTED;
    return esp_wifi_register_eapol_txdonecb_internal(eapol_sent) == ESP_OK &&
           esp_wifi_register_mgmt_frame_internal(1u << WLAN_FC_STYPE_ASSOC_RESP | 1u << WLAN_FC_STYPE_REASSOC_RESP,
                                                 0) == ESP_OK;
}

static bool sta_deinit(void)
{
    esp_wifi_register_eapol_txdonecb_internal(0);
    drop_held();
    wpa_sm_deinit(supp.sm);
    supp.sm = 0;
    return true;
}

/*
 * The libraries chose an access point and ask what to associate with:
 * the suites of the profile, the RSN element that says them, and the PMK.
 * The supplicant takes WPA2's personal RSN, with a passphrase or a PSK,
 * and lets an open network through, which the libraries refuse before they ask, see TODO.md;
 * WPA3's SAE is not done yet, nor WPA's, WAPI or any enterprise.
 */
static int sta_connect(uint8_t *bssid)
{
    struct wpa_sm *sm = supp.sm;
    uint8_t authmode = esp_wifi_sta_get_prof_authmode_internal();
    os_memcpy(supp.bssid, bssid, ETH_ALEN);
    drop_held();
    if (authmode == ESP_AUTH_NONE) {
        esp_wifi_unset_appie_internal(WIFI_APPIE_RSN);
        return esp_wifi_sta_connect_internal(bssid);
    }
    int key_mgmt = authmode == ESP_AUTH_WPA2_PSK          ? WPA_KEY_MGMT_PSK
                   : authmode == ESP_AUTH_WPA2_PSK_SHA256 ? WPA_KEY_MGMT_PSK_SHA256
                                                          : 0;
    if (!esp_wifi_sta_prof_is_rsn_internal() || key_mgmt == 0) {
        wpa_printf(MSG_ERROR, "supp: authentication mode %u is not supported", authmode);
        return -1;
    }
    int pmf = esp_wifi_sta_pmf_enabled() != 0;
    wpa_sm_set_param(sm, WPA_PARAM_PROTO, WPA_PROTO_RSN);
    wpa_sm_set_param(sm, WPA_PARAM_RSN_ENABLED, 1);
    wpa_sm_set_param(sm, WPA_PARAM_KEY_MGMT, (unsigned)key_mgmt);
    int pairwise = cipher_from_esp_bit(esp_wifi_sta_get_pairwise_cipher_internal());
    wpa_sm_set_param(sm, WPA_PARAM_PAIRWISE, (unsigned)pairwise);
    wpa_sm_set_param(sm, WPA_PARAM_GROUP, (unsigned)cipher_from_esp_bit(esp_wifi_sta_get_group_cipher_internal()));
    wpa_sm_set_param(sm, WPA_PARAM_MFP, pmf ? MGMT_FRAME_PROTECTION_OPTIONAL : NO_MGMT_FRAME_PROTECTION);
    wpa_sm_set_param(sm, WPA_PARAM_MGMT_GROUP,
                     pmf ? (unsigned)cipher_from_esp(esp_wifi_sta_get_mgmt_group_cipher()) : 0);
    const struct wifi_ssid *ssid = esp_wifi_sta_get_prof_ssid_internal();
    size_t ssid_len = ssid->len < 0 || ssid->len > SSID_MAX_LEN ? 0 : (size_t)ssid->len;
    wpa_sm_set_ssid(sm, ssid->ssid, ssid_len);
    ap_ies();

    char pass[65];
    os_memcpy(pass, esp_wifi_sta_get_prof_password_internal(), 64);
    pass[64] = 0;
    int bad = pmk_for(pass, ssid->ssid, ssid_len);
    forced_memzero(pass, sizeof(pass));
    if (bad) {
        wpa_printf(MSG_ERROR, "supp: the passphrase is neither 8 to 63 characters nor 64 hexadecimal digits");
        return -1;
    }
    wpa_sm_set_pmk(sm, supp.pmk, PMK_LEN, 0, 0);

    /* hostap keeps the element of an earlier association unless told to drop it, and would send that in message 2/4. */
    u8 ie[80];
    size_t ie_len = sizeof(ie);
    wpa_sm_set_assoc_wpa_ie(sm, 0, 0);
    if (wpa_sm_set_assoc_wpa_ie_default(sm, ie, &ie_len) < 0) {
        wpa_printf(MSG_ERROR, "supp: no RSN element for the association");
        return -1;
    }
    /*
     * Copied: with a flag of 1 the libraries would keep a pointer to the buffer instead,
     * read the element from its third byte on, and write its length over the first two, as ESP-IDF's fork lays it out.
     */
    esp_wifi_set_appie_internal(WIFI_APPIE_RSN, ie, (uint16_t)ie_len, 0);
    wpa_printf(MSG_DEBUG, "supp: to " MACSTR " with AKM 0x%x, pairwise 0x%x, PMF %d", MAC2STR(bssid), key_mgmt,
               pairwise, pmf);
    return esp_wifi_sta_connect_internal(bssid);
}

static void sta_connected(uint8_t *bssid)
{
    wpa_printf(MSG_DEBUG, "supp: connected to " MACSTR, MAC2STR(bssid));
}

static void sta_disconnected(uint8_t reason)
{
    wpa_printf(MSG_DEBUG, "supp: disconnected, reason %u", reason);
    drop_held();
    wpa_sm_notify_disassoc(supp.sm);
}

static int sta_rx_eapol(uint8_t *src, uint8_t *buf, uint32_t len)
{
    return wpa_sm_rx_eapol(supp.sm, src, buf, len, FRAME_ENCRYPTION_UNKNOWN);
}

/* Whether the 4-way handshake runs, message 4/4 not yet sent: the libraries send its frames unencrypted. */
static bool sta_in_4way(void)
{
    return supp.state == WPA_4WAY_HANDSHAKE || supp.m4_out;
}

/* The management frames the supplicant registered for: an association's response starts a handshake. */
static int sta_rx_mgmt(uint8_t type, uint8_t *frame, size_t len, uint8_t *sender, int8_t rssi, uint8_t channel,
                       uint64_t tsf)
{
    (void)frame, (void)len, (void)rssi, (void)channel, (void)tsf;
    if (type == WLAN_FC_STYPE_ASSOC_RESP || type == WLAN_FC_STYPE_REASSOC_RESP) {
        drop_held();
        os_memcpy(supp.bssid, sender, ETH_ALEN);
        wpa_sm_notify_assoc(supp.sm, sender);
    }
    return 0;
}

/*
 * An access point's RSN, RSNX or WPA element as the libraries read it, for a scan or a connection:
 * hostap parses the RSN and WPA ones, the supplicant the first octet of the RSNX one's capabilities.
 */
static int parse_wpa_ie(const uint8_t *ie, size_t len, struct wifi_wpa_ie *data)
{
    struct wpa_ie_data d = { 0 };
    os_memset(data, 0, sizeof(*data));
    if (len >= 3 && ie[0] == WLAN_EID_RSNX) {
        data->rsnxe_capa = ie[2];
        return 0;
    }
    if (len >= 7 && ie[0] == WLAN_EID_VENDOR_SPECIFIC && ie[1] >= 5 &&
        WPA_GET_BE32(&ie[2]) == RSNXE_OVERRIDE_IE_VENDOR_TYPE) {
        data->rsnxe_capa = ie[6];
        return 0;
    }
    if (len >= 1 && ie[0] == WLAN_EID_BSS_AC_ACCESS_DELAY) { /* WAPI's, which the supplicant does not do */
        return 0;
    }
    int r = wpa_parse_wpa_ie(ie, len, &d);
    data->proto = d.proto;
    data->pairwise_cipher = cipher_to_esp(d.pairwise_cipher);
    data->group_cipher = cipher_to_esp(d.group_cipher);
    data->key_mgmt = d.key_mgmt & ESP_KEY_MGMT_KNOWN;
    data->capabilities = d.capabilities;
    data->num_pmkid = d.num_pmkid;
    data->pmkid = d.pmkid;
    data->mgmt_group_cipher = cipher_to_esp(d.mgmt_group_cipher);
    return r;
}

static int michael_mic_failure(uint16_t unicast)
{
    wpa_sm_key_request(supp.sm, 1, unicast);
    return 0;
}

static void clear_pmksa(void)
{
    pmksa_cache_clear_current(supp.sm);
}

static void config_reload(void)
{
    wpa_sm_pmksa_cache_flush(supp.sm, 0);
}

static void config_done(void)
{
}

/* The table, of which drv_wpa_register hands the libraries a copy in the heap: they free it when it is unregistered. */
static const struct wpa_funcs wpa = {
    .wpa_sta_init = sta_init,
    .wpa_sta_deinit = sta_deinit,
    .wpa_sta_connect = sta_connect,
    .wpa_sta_connected_cb = sta_connected,
    .wpa_sta_disconnected_cb = sta_disconnected,
    .wpa_sta_rx_eapol = sta_rx_eapol,
    .wpa_sta_in_4way_handshake = sta_in_4way,
    .wpa_parse_wpa_ie = parse_wpa_ie,
    .wpa_michael_mic_failure = michael_mic_failure,
    .wpa_sta_rx_mgmt = sta_rx_mgmt,
    .wpa_config_done = config_done,
    .wpa_sta_clear_curr_pmksa = clear_pmksa,
    .wpa_config_reload = config_reload,
    .wpa_parse_wpa_ie_scan_only = parse_wpa_ie,
};

esp_err_t drv_wpa_register(void)
{
    struct wpa_funcs *t = os_malloc(sizeof(*t));
    if (t == 0) {
        return ESP_FAIL;
    }
    *t = wpa;
    return esp_wifi_register_wpa_cb_internal(t);
}
