/*
 * The supplicant, supp.h, behind the table Espressif's libraries call a supplicant through, struct wpa_funcs of esp.h,
 * which ESP-IDF's esp_wpa_main.c fills for its fork: the libraries as the supplicant's link.
 *
 * The libraries pick the access point, authenticate and associate, and encrypt in the MAC.
 * They ask the supplicant for the RSN element to associate with, and are called back to go on;
 * they hand it the EAPOL frames of the handshakes, send its own, and install the keys it derives.
 * They call the table on their Wi-Fi task, so the supplicant runs there, its timeouts included.
 *
 * The libraries send a frame some time after they are handed it, with whichever key is installed then,
 * where hostap's drivers keep the order: hostap installs the pairwise key for sending right after message 4/4,
 * which would then leave encrypted with a key the access point installs only once it has the message.
 * So, as ESP-IDF's fork does, what hostap installs after handing over message 4/4 is held back,
 * and installed once the libraries report the message sent.
 */

#include "utils/common.h"

#include "common/defs.h"
#include "common/eapol_common.h"
#include "common/ieee802_11_defs.h"
#include "common/wpa_common.h"
#include "rsn_supp/wpa.h"

#include "esp.h"
#include "osi.h"
#include "supp.h"

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
    u8 own[ETH_ALEN], bssid[ETH_ALEN];
    int m4_out;        /* message 4/4 handed to the libraries, not yet reported sent */
    int complete_held; /* the handshake completed while message 4/4 was out */
    struct held held[HELD_MAX];
    unsigned held_count;
} wpa;

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

/* SAE's suites the supplicant does not take, hidden so that the libraries do not choose them. */
#define KEY_MGMT_SAE_OTHER (WPA_KEY_MGMT_FT_SAE | WPA_KEY_MGMT_SAE_EXT_KEY | WPA_KEY_MGMT_FT_SAE_EXT_KEY)

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
    forced_memzero(wpa.held, sizeof(wpa.held));
    wpa.held_count = 0;
    wpa.m4_out = 0;
    wpa.complete_held = 0;
}

static int set_key(const struct supp_key *s)
{
    /*
     * The pairwise key for receiving alone, before message 4/4, which hostap installs to receive with it at once,
     * and lets fail, as many drivers do not take it: the libraries take the pairwise key once, for both ways,
     * and one installed for receiving first then never encrypts what they send.
     */
    if (s->flag & KEY_FLAG_NEXT) {
        return 0;
    }
    struct held k = { s->alg, { 0 }, s->idx, s->set_tx, { 0 }, s->seq_len, { 0 }, s->key_len, s->flag };
    if (s->seq_len > sizeof(k.seq) || s->key_len > sizeof(k.key)) {
        wpa_printf(MSG_ERROR, "supp: a key longer than any the libraries take");
        return -1;
    }
    /* The libraries file every key of the station's under the access point's address, a group key too. */
    os_memcpy(k.addr, (s->flag & KEY_FLAG_GROUP) || s->addr == 0 ? wpa.bssid : s->addr, ETH_ALEN);
    if (s->seq) {
        os_memcpy(k.seq, s->seq, s->seq_len);
    }
    if (s->key) {
        os_memcpy(k.key, s->key, s->key_len);
    }
    /*
     * A TKIP group key is the temporal key, then the access point's Michael key for sending, then for receiving.
     * hostap swaps the two Michael keys, so that the one the station checks with lies where Linux's drivers read
     * a key to receive with; the libraries read 802.11's order, as ESP-IDF's fork leaves it, so they go back.
     * Swapped, every frame to the group fails Michael's check, and two in a minute end the association, reason 14.
     */
    if (s->alg == WPA_ALG_TKIP && (s->flag & KEY_FLAG_GROUP) && s->key_len == 32) {
        for (unsigned i = 16; i < 24; i++) {
            u8 t = k.key[i];
            k.key[i] = k.key[i + 8];
            k.key[i + 8] = t;
        }
    }
    int r = 0;
    if (!wpa.m4_out) {
        r = install(&k);
    } else if (wpa.held_count < HELD_MAX) {
        wpa.held[wpa.held_count++] = k;
    } else {
        wpa_printf(MSG_ERROR, "supp: more keys after message 4/4 than the supplicant holds");
        r = -1;
    }
    forced_memzero(&k, sizeof(k));
    return r;
}

static void completed(void)
{
    if (wpa.m4_out) {
        wpa.complete_held = 1;
    } else {
        esp_wifi_auth_done_internal();
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
    if (!wpa.m4_out || !is_4_of_4(buf, len)) {
        return;
    }
    if (failed) {
        /* The access point sends message 3/4 again, and hostap message 4/4, which leaves held what is held. */
        wpa_printf(MSG_INFO, "supp: message 4/4 was not sent; the keys wait for the next");
        return;
    }
    wpa.m4_out = 0;
    for (unsigned i = 0; i < wpa.held_count; i++) {
        if (install(&wpa.held[i]) < 0) {
            drop_held();
            esp_wifi_deauthenticate_internal(WLAN_REASON_UNSPECIFIED);
            return;
        }
    }
    int complete = wpa.complete_held;
    drop_held();
    if (complete) {
        esp_wifi_auth_done_internal();
    }
}

/* The libraries take an Ethernet frame: destination, source, type, and the EAPOL frame. */
static int send_frame(const u8 *dest, u16 proto, const u8 *buf, size_t len)
{
    if (len > 0xffff - 14) {
        return -1;
    }
    u8 *frame = os_malloc(14 + len);
    if (frame == 0) {
        return -1;
    }
    os_memcpy(frame, dest, ETH_ALEN);
    os_memcpy(frame + 6, wpa.own, ETH_ALEN);
    WPA_PUT_BE16(frame + 12, proto);
    os_memcpy(frame + 14, buf, len);
    if (proto == ETH_P_EAPOL && is_4_of_4(buf, len)) {
        wpa.m4_out = 1;
    }
    int r = esp_wifi_internal_tx(WIFI_IF_STA, frame, (uint16_t)(14 + len));
    os_free(frame);
    return r == 0 ? 0 : -1;
}

static void deauthenticate(uint16_t reason)
{
    esp_wifi_deauthenticate_internal((uint8_t)reason);
}

static void run(int (*fn)(void *arg), void *arg)
{
    struct wifi_ipc ipc = { fn, arg, 0 };
    esp_wifi_ipc_internal(&ipc, false);
}

static const struct supp_link libraries = { send_frame, set_key, completed, deauthenticate, run };

/* --- The table the libraries call. --- */

static bool sta_init(void)
{
    esp_wifi_get_macaddr_internal(WIFI_IF_STA, wpa.own);
    return supp_init(&libraries, wpa.own) == 0 && esp_wifi_register_eapol_txdonecb_internal(eapol_sent) == ESP_OK &&
           esp_wifi_register_mgmt_frame_internal(1u << WLAN_FC_STYPE_ASSOC_RESP | 1u << WLAN_FC_STYPE_REASSOC_RESP,
                                                 0) == ESP_OK;
}

static bool sta_deinit(void)
{
    esp_wifi_register_eapol_txdonecb_internal(0);
    drop_held();
    supp_deinit();
    return true;
}

/*
 * The libraries chose an access point and ask what to associate with:
 * the suites of the profile, which the supplicant turns into the RSN and RSNX elements that say them, and the PMK.
 * An open network is let through, which the libraries refuse before they ask, see TODO.md.
 */
static int sta_connect(uint8_t *bssid)
{
    uint8_t authmode = esp_wifi_sta_get_prof_authmode_internal();
    os_memcpy(wpa.bssid, bssid, ETH_ALEN);
    drop_held();
    if (authmode == ESP_AUTH_NONE) {
        esp_wifi_unset_appie_internal(WIFI_APPIE_RSN);
        esp_wifi_unset_appie_internal(WIFI_APPIE_ASSOC_REQ);
        return esp_wifi_sta_connect_internal(bssid);
    }
    int key_mgmt = authmode == ESP_AUTH_WPA2_PSK          ? WPA_KEY_MGMT_PSK
                   : authmode == ESP_AUTH_WPA2_PSK_SHA256 ? WPA_KEY_MGMT_PSK_SHA256
                   : authmode == ESP_AUTH_WPA3_PSK        ? WPA_KEY_MGMT_SAE
                                                          : 0;
    if (!esp_wifi_sta_prof_is_rsn_internal() || key_mgmt == 0) {
        wpa_printf(MSG_ERROR, "supp: authentication mode %u is not supported", authmode);
        return -1;
    }
    int pmf = esp_wifi_sta_pmf_enabled() != 0;
    const struct wifi_ssid *ssid = esp_wifi_sta_get_prof_ssid_internal();
    char pass[65];
    os_memcpy(pass, esp_wifi_sta_get_prof_password_internal(), 64);
    pass[64] = 0;
    const struct supp_network n = {
        .bssid = bssid,
        .ssid = ssid->ssid,
        .ssid_len = ssid->len < 0 || ssid->len > SSID_MAX_LEN ? 0 : (size_t)ssid->len,
        .pass = pass,
        .key_mgmt = key_mgmt,
        .pairwise = cipher_from_esp_bit(esp_wifi_sta_get_pairwise_cipher_internal()),
        .group = cipher_from_esp_bit(esp_wifi_sta_get_group_cipher_internal()),
        .pmf = pmf,
        .mgmt_group = pmf ? cipher_from_esp(esp_wifi_sta_get_mgmt_group_cipher()) : 0,
        .sae_pwe = esp_wifi_get_config_sae_pwe_h2e_internal(WIFI_IF_STA),
        .ap_rsn = esp_wifi_sta_get_ie(wpa.bssid, WLAN_EID_RSN),
        .ap_rsnx = esp_wifi_sta_get_ie(wpa.bssid, WLAN_EID_RSNX),
    };
    u8 rsn[80], rsnx[8];
    size_t rsn_len = sizeof(rsn), rsnx_len = sizeof(rsnx);
    int bad = supp_connect(&n, rsn, &rsn_len, rsnx, &rsnx_len);
    forced_memzero(pass, sizeof(pass));
    if (bad) {
        return -1;
    }
    if (rsnx_len > 0) {
        esp_wifi_set_appie_internal(WIFI_APPIE_ASSOC_REQ, rsnx, (uint16_t)rsnx_len, 0);
    } else {
        esp_wifi_unset_appie_internal(WIFI_APPIE_ASSOC_REQ);
    }
    /*
     * Copied: with a flag of 1 the libraries would keep a pointer to the buffer instead,
     * read the element from its third byte on, and write its length over the first two, as ESP-IDF's fork lays it out.
     */
    esp_wifi_set_appie_internal(WIFI_APPIE_RSN, rsn, (uint16_t)rsn_len, 0);
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
    supp_disassociated();
}

static int sta_rx_eapol(uint8_t *src, uint8_t *buf, uint32_t len)
{
    return supp_rx_eapol(src, buf, len);
}

/* Whether the 4-way handshake runs, message 4/4 not yet sent: the libraries send its frames unencrypted. */
static bool sta_in_4way(void)
{
    return supp_in_4way() || wpa.m4_out;
}

/* The management frames the supplicant registered for: an association's response starts a handshake. */
static int sta_rx_mgmt(uint8_t type, uint8_t *frame, size_t len, uint8_t *sender, int8_t rssi, uint8_t channel,
                       uint64_t tsf)
{
    (void)frame, (void)len, (void)rssi, (void)channel, (void)tsf;
    if (type == WLAN_FC_STYPE_ASSOC_RESP || type == WLAN_FC_STYPE_REASSOC_RESP) {
        drop_held();
        os_memcpy(wpa.bssid, sender, ETH_ALEN);
        supp_associated(sender);
    }
    return 0;
}

/* SAE's messages, which the libraries copy into authentication frames, and those they received, in their terms. */
static uint8_t *sae_build(uint8_t *bssid, uint32_t type, size_t *len)
{
    *len = 0;
    return (uint8_t *)(type == SAE_MSG_COMMIT    ? supp_sae_commit(bssid, len)
                       : type == SAE_MSG_CONFIRM ? supp_sae_confirm(len)
                                                 : 0);
}

static int sae_parse(uint8_t *buf, size_t len, uint32_t type, uint16_t status)
{
    int r = type == SAE_MSG_COMMIT    ? supp_sae_take_commit(buf, len, status)
            : type == SAE_MSG_CONFIRM ? supp_sae_take_confirm(buf, len)
                                      : SUPP_FAILED;
    return r == SUPP_TAKEN ? ESP_OK : r == SUPP_FAILED ? ESP_FAIL : r == SUPP_DISCARD ? ESP_ERR_WIFI_DISCARD : r;
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
    data->key_mgmt = d.key_mgmt & ESP_KEY_MGMT_KNOWN & ~KEY_MGMT_SAE_OTHER;
    data->capabilities = d.capabilities;
    data->num_pmkid = d.num_pmkid;
    data->pmkid = d.pmkid;
    data->mgmt_group_cipher = cipher_to_esp(d.mgmt_group_cipher);
    return r;
}

static int michael_mic_failure(uint16_t unicast)
{
    supp_michael_failed(unicast);
    return 0;
}

/*
 * What the supplicant need not do: the libraries' end of a configuration, and their asks to forget PMKSAs,
 * which it keeps none of: hostap caches one for EAP and Suite B alone, neither built.
 */
static void nothing(void)
{
}

/* The table, of which drv_wpa_register hands the libraries a copy in the heap: they free it when it is unregistered. */
static const struct wpa_funcs funcs = {
    .wpa_sta_init = sta_init,
    .wpa_sta_deinit = sta_deinit,
    .wpa_sta_connect = sta_connect,
    .wpa_sta_connected_cb = sta_connected,
    .wpa_sta_disconnected_cb = sta_disconnected,
    .wpa_sta_rx_eapol = sta_rx_eapol,
    .wpa_sta_in_4way_handshake = sta_in_4way,
    .wpa_parse_wpa_ie = parse_wpa_ie,
    .wpa_michael_mic_failure = michael_mic_failure,
    .wpa3_build_sae_msg = sae_build,
    .wpa3_parse_sae_msg = sae_parse,
    .wpa_sta_rx_mgmt = sta_rx_mgmt,
    .wpa_config_done = nothing,
    .wpa_sta_clear_curr_pmksa = nothing,
    .wpa_config_reload = nothing,
    .wpa_parse_wpa_ie_scan_only = parse_wpa_ie,
};

esp_err_t drv_wpa_register(void)
{
    struct wpa_funcs *t = os_malloc(sizeof(*t));
    if (t == 0) {
        return ESP_FAIL;
    }
    *t = funcs;
    return esp_wifi_register_wpa_cb_internal(t);
}
