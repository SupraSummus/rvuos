/*
 * The supplicant, hostap's over a station's link; see supp.h.
 * What hostap asks of its driver goes to the link, the frames it sends, the keys it installs and the end of the handshakes;
 * the rest it asks is answered here, from what supp_connect was given.
 */

#include "utils/common.h"

#include "common/defs.h"
#include "common/eapol_common.h"
#include "common/ieee802_11_common.h"
#include "common/ieee802_11_defs.h"
#include "common/sae.h"
#include "common/wpa_common.h"
#include "crypto/sha1.h"
#include "rsn_supp/wpa.h"
#include "utils/eloop.h"

#include "supp.h"

/* EAPOL's version the supplicant sends, wpa_supplicant's default: version 2 is dropped by some access points. */
#define EAPOL_VERSION_SENT 1

static struct {
    const struct supp_link *link;
    struct wpa_sm *sm;
    enum wpa_states state;
    u8 own[ETH_ALEN], bssid[ETH_ALEN];

    /* The network supp_connect was given, which SAE reads. */
    u8 ssid[SSID_MAX_LEN];
    size_t ssid_len;
    char pass[65];

    /* The PMK of the passphrase and SSID last asked, which costs a fifth of a second to derive. */
    u8 pmk[PMK_LEN];
    u8 pmk_ssid[SSID_MAX_LEN];
    size_t pmk_ssid_len;
    char pmk_pass[65];

    /* SAE with the access point being joined, by hash to element or not, and the message last built. */
    struct sae_data sae;
    int sae_h2e;
    struct sae_pt *sae_pt;
    struct wpabuf *sae_token, *sae_msg;
} supp;

/* --- What hostap asks of its driver. --- */

static int set_key(void *ctx, int link_id, enum wpa_alg alg, const u8 *addr, int key_idx, int set_tx, const u8 *seq,
                   size_t seq_len, const u8 *key, size_t key_len, enum key_flag key_flag)
{
    (void)ctx;
    (void)link_id;
    const struct supp_key k = { alg, addr, key_idx, set_tx, seq, seq_len, key, key_len, key_flag };
    return supp.link->set_key(&k);
}

static void set_state(void *ctx, enum wpa_states state)
{
    (void)ctx;
    supp.state = state;
    if (state == WPA_COMPLETED) {
        supp.link->completed();
    }
}

static enum wpa_states get_state(void *ctx)
{
    (void)ctx;
    return supp.state;
}

static void deauthenticate(void *ctx, u16 reason)
{
    (void)ctx;
    supp.link->deauthenticate(reason);
}

static void reconnect(void *ctx)
{
    (void)ctx;
    supp.link->deauthenticate(WLAN_REASON_DEAUTH_LEAVING);
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

static int ether_send(void *ctx, const u8 *dest, u16 proto, const u8 *buf, size_t len)
{
    (void)ctx;
    return supp.link->send(dest, proto, buf, len);
}

/* The access point's elements are those supp_connect was given; with none, there are none to find later either. */
static int get_beacon_ie(void *ctx)
{
    (void)ctx;
    return -1;
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

/*
 * No station keeps a PMKSA cache of its own, nor protection hostap could switch on and off;
 * hostap's cache is EAPOL's, which is not built, so its functions here do nothing.
 */
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

void supp_prepare(const char *ssid, const char *pass)
{
    pmk_for(pass, (const u8 *)ssid, strnlen(ssid, SSID_MAX_LEN));
}

/* --- WPA3's SAE. --- */

/* The milliseconds since start, for the log. */
static unsigned ms_since(struct os_reltime *start)
{
    struct os_reltime now, d;
    os_get_reltime(&now);
    os_reltime_sub(&now, start, &d);
    return (unsigned)(d.sec * 1000 + d.usec / 1000);
}

/* The groups SAE takes, P-256's alone. */
static int sae_groups[] = { 19, 0 };

static void sae_free(void)
{
    sae_clear_data(&supp.sae);
    sae_deinit_pt(supp.sae_pt);
    supp.sae_pt = 0;
    wpabuf_free(supp.sae_token);
    supp.sae_token = 0;
    wpabuf_free(supp.sae_msg);
    supp.sae_msg = 0;
}

/* The message just built; it lasts until the next. */
static const uint8_t *sae_built(size_t *len)
{
    *len = wpabuf_len(supp.sae_msg);
    return wpabuf_head_u8(supp.sae_msg);
}

const uint8_t *supp_sae_commit(const uint8_t *bssid, size_t *len)
{
    struct os_reltime start;
    os_get_reltime(&start);
    *len = 0;
    if (supp.sae_token == 0) {
        size_t pass_len = os_strlen(supp.pass);
        sae_clear_data(&supp.sae);
        if (sae_set_group(&supp.sae, sae_groups[0]) < 0) {
            return 0;
        }
        supp.sae.akmp = WPA_KEY_MGMT_SAE;
        if (supp.sae_h2e) {
            if (supp.sae_pt == 0) {
                supp.sae_pt = sae_derive_pt(sae_groups, supp.ssid, supp.ssid_len, (const u8 *)supp.pass, pass_len, 0, 0);
            }
            if (supp.sae_pt == 0 || sae_prepare_commit_pt(&supp.sae, supp.sae_pt, supp.own, bssid, 0, 0) < 0) {
                wpa_printf(MSG_ERROR, "supp: no SAE commit by hash to element");
                return 0;
            }
        } else if (sae_prepare_commit(supp.own, bssid, (const u8 *)supp.pass, pass_len, &supp.sae) < 0) {
            wpa_printf(MSG_ERROR, "supp: no SAE commit by hunting and pecking");
            return 0;
        }
    }
    wpabuf_free(supp.sae_msg);
    supp.sae_msg = wpabuf_alloc(SAE_COMMIT_MAX_LEN + (supp.sae_token ? wpabuf_len(supp.sae_token) : 0));
    if (supp.sae_msg == 0 || sae_write_commit(&supp.sae, supp.sae_msg, supp.sae_token, 0, 0) < 0) {
        return 0;
    }
    wpabuf_free(supp.sae_token);
    supp.sae_token = 0;
    supp.sae.state = SAE_COMMITTED;
    wpa_printf(MSG_INFO, "supp: SAE's commit to " MACSTR " by %s, in %u ms", MAC2STR(bssid),
               supp.sae_h2e ? "hash to element" : "hunting and pecking", ms_since(&start));
    return sae_built(len);
}

const uint8_t *supp_sae_confirm(size_t *len)
{
    *len = 0;
    if (supp.sae.state != SAE_COMMITTED) {
        return 0;
    }
    wpabuf_free(supp.sae_msg);
    supp.sae_msg = wpabuf_alloc(SAE_CONFIRM_MAX_LEN);
    if (supp.sae_msg == 0 || sae_write_confirm(&supp.sae, supp.sae_msg) < 0) {
        return 0;
    }
    supp.sae.state = SAE_CONFIRMED;
    return sae_built(len);
}

/* The access point's request for a token: the group, then the token, by hash to element in a container element. */
static int sae_take_token(const uint8_t *buf, size_t len)
{
    if (len < 2 || WPA_GET_LE16(buf) != sae_groups[0]) {
        return SUPP_FAILED;
    }
    const u8 *token = buf + 2;
    size_t token_len = len - 2;
    if (supp.sae.h2e) {
        if (token_len < 3 || token[0] != WLAN_EID_EXTENSION || token[1] < 1 || token[1] > token_len - 2 ||
            token[2] != WLAN_EID_EXT_ANTI_CLOGGING_TOKEN) {
            wpa_printf(MSG_ERROR, "supp: an SAE token's container that is not one");
            return SUPP_FAILED;
        }
        token_len = token[1] - 1u;
        token += 3;
    }
    wpabuf_free(supp.sae_token);
    supp.sae_token = wpabuf_alloc_copy(token, token_len);
    return supp.sae_token ? SUPP_TAKEN : SUPP_FAILED;
}

/*
 * A commit that is ours sent back is dropped, as is one in another state.
 * With P-256 alone there is no weaker group to be led to, so the access point's list of refused groups goes unread.
 */
int supp_sae_take_commit(const uint8_t *buf, size_t len, uint16_t status)
{
    if (supp.sae.state != SAE_COMMITTED) {
        return SUPP_DISCARD;
    }
    if (status == WLAN_STATUS_ANTI_CLOGGING_TOKEN_REQ) {
        return sae_take_token(buf, len);
    }
    struct os_reltime start;
    os_get_reltime(&start);
    int h2e = status == WLAN_STATUS_SAE_HASH_TO_ELEMENT || status == WLAN_STATUS_SAE_PK;
    u16 r = sae_parse_commit(&supp.sae, buf, len, 0, 0, sae_groups, h2e, 0);
    if (r == SAE_SILENTLY_DISCARD) {
        return SUPP_DISCARD;
    }
    if (r != WLAN_STATUS_SUCCESS) {
        wpa_printf(MSG_ERROR, "supp: the access point's SAE commit refused: status %u", r);
        return r;
    }
    if (sae_process_commit(&supp.sae) < 0) {
        wpa_printf(MSG_ERROR, "supp: no keys of the access point's SAE commit");
        return SUPP_FAILED;
    }
    wpa_printf(MSG_INFO, "supp: the access point's SAE commit taken, in %u ms",
               ms_since(&start));
    return SUPP_TAKEN;
}

/*
 * The access point's confirm, which shows it knows the password: SAE's PMK is then the handshake's.
 * With no PMKSA cache, hostap says it finds no PMKID for message 1/4, and goes on.
 */
int supp_sae_take_confirm(const uint8_t *buf, size_t len)
{
    int r = SUPP_FAILED;
    if (supp.sae.state == SAE_CONFIRMED && sae_check_confirm(&supp.sae, buf, len, 0) == 0) {
        supp.sae.state = SAE_ACCEPTED;
        wpa_sm_set_pmk(supp.sm, supp.sae.pmk, supp.sae.pmk_len, 0, 0);
        r = SUPP_TAKEN;
    }
    wpa_printf(r == SUPP_TAKEN ? MSG_INFO : MSG_ERROR, "supp: the access point's SAE confirm %s",
               r == SUPP_TAKEN ? "taken" : "refused");
    sae_free();
    return r;
}

/* --- The station's side. --- */

int supp_init(const struct supp_link *link, const uint8_t *own)
{
    if (supp.sm == 0) {
        struct wpa_sm_ctx *ctx = os_memdup(&sm_ctx, sizeof(sm_ctx));
        if (ctx == 0 || (supp.sm = wpa_sm_init(ctx)) == 0) {
            os_free(ctx);
            return -1;
        }
    }
    supp.link = link;
    os_memcpy(supp.own, own, ETH_ALEN);
    wpa_sm_set_own_addr(supp.sm, supp.own);
    supp.state = WPA_DISCONNECTED;
    return 0;
}

void supp_deinit(void)
{
    sae_free();
    forced_memzero(supp.pass, sizeof(supp.pass));
    wpa_sm_deinit(supp.sm);
    supp.sm = 0;
}

int supp_choose(struct supp_network *n)
{
    struct wpa_ie_data d;
    if (n->ap_rsn == 0 || wpa_parse_wpa_ie_rsn(n->ap_rsn, 2u + n->ap_rsn[1], &d) < 0) {
        wpa_printf(MSG_ERROR, "supp: the access point's RSN element is not one");
        return -1;
    }
    if (!(d.key_mgmt & WPA_KEY_MGMT_PSK) || !(d.pairwise_cipher & WPA_CIPHER_CCMP) ||
        d.group_cipher != WPA_CIPHER_CCMP || (d.capabilities & WPA_CAPABILITY_MFPR)) {
        wpa_printf(MSG_ERROR, "supp: the access point offers AKM 0x%x, pairwise 0x%x, group 0x%x, capabilities 0x%x",
                   d.key_mgmt, d.pairwise_cipher, d.group_cipher, d.capabilities);
        return -1;
    }
    n->key_mgmt = WPA_KEY_MGMT_PSK;
    n->pairwise = WPA_CIPHER_CCMP;
    n->group = WPA_CIPHER_CCMP;
    n->pmf = 0;
    n->mgmt_group = 0;
    return 0;
}

/*
 * The supplicant takes WPA2's personal RSN, with a passphrase or a PSK, and WPA3's, with SAE,
 * whose PMK SAE derives before the association, and which needs management frames protected;
 * WPA's is not done, nor WAPI or any enterprise.
 */
int supp_connect(const struct supp_network *n, uint8_t *rsn, size_t *rsn_len, uint8_t *rsnx, size_t *rsnx_len)
{
    struct wpa_sm *sm = supp.sm;
    os_memcpy(supp.bssid, n->bssid, ETH_ALEN);
    sae_free();
    if (n->key_mgmt != WPA_KEY_MGMT_PSK && n->key_mgmt != WPA_KEY_MGMT_PSK_SHA256 && n->key_mgmt != WPA_KEY_MGMT_SAE) {
        wpa_printf(MSG_ERROR, "supp: key management 0x%x is not supported", n->key_mgmt);
        return -1;
    }
    if (n->key_mgmt == WPA_KEY_MGMT_SAE && !n->pmf) {
        wpa_printf(MSG_ERROR, "supp: SAE needs management frames protected, which the station does not offer");
        return -1;
    }
    size_t ssid_len = n->ssid_len > SSID_MAX_LEN ? 0 : n->ssid_len;
    os_memcpy(supp.ssid, n->ssid, ssid_len);
    supp.ssid_len = ssid_len;
    size_t pass_len = strnlen(n->pass, 64);
    os_memcpy(supp.pass, n->pass, pass_len);
    supp.pass[pass_len] = 0;

    wpa_sm_set_param(sm, WPA_PARAM_PROTO, WPA_PROTO_RSN);
    wpa_sm_set_param(sm, WPA_PARAM_RSN_ENABLED, 1);
    wpa_sm_set_param(sm, WPA_PARAM_KEY_MGMT, (unsigned)n->key_mgmt);
    wpa_sm_set_param(sm, WPA_PARAM_PAIRWISE, (unsigned)n->pairwise);
    wpa_sm_set_param(sm, WPA_PARAM_GROUP, (unsigned)n->group);
    wpa_sm_set_param(sm, WPA_PARAM_MFP, n->pmf ? MGMT_FRAME_PROTECTION_OPTIONAL : NO_MGMT_FRAME_PROTECTION);
    wpa_sm_set_param(sm, WPA_PARAM_MGMT_GROUP, n->pmf ? (unsigned)n->mgmt_group : 0);
    wpa_sm_set_ssid(sm, supp.ssid, ssid_len);
    wpa_sm_set_ap_rsn_ie(sm, n->ap_rsn, n->ap_rsn ? 2u + n->ap_rsn[1] : 0);
    wpa_sm_set_ap_rsnxe(sm, n->ap_rsnx, n->ap_rsnx ? 2u + n->ap_rsnx[1] : 0);

    if (n->key_mgmt != WPA_KEY_MGMT_SAE) {
        if (pmk_for(supp.pass, supp.ssid, ssid_len)) {
            wpa_printf(MSG_ERROR, "supp: the passphrase is neither 8 to 63 characters nor 64 hexadecimal digits");
            return -1;
        }
        wpa_sm_set_pmk(sm, supp.pmk, PMK_LEN, 0, 0);
    }

    /* SAE by hash to element where the station and the access point's RSNX element allow it, which the station's says. */
    supp.sae_h2e = n->key_mgmt == WPA_KEY_MGMT_SAE &&
                   (n->sae_pwe == SAE_PWE_HASH_TO_ELEMENT || n->sae_pwe == SAE_PWE_BOTH) &&
                   ieee802_11_rsnx_capab(n->ap_rsnx, WLAN_RSNX_CAPAB_SAE_H2E);
    wpa_sm_set_param(sm, WPA_PARAM_SAE_PWE, supp.sae_h2e ? SAE_PWE_HASH_TO_ELEMENT : SAE_PWE_HUNT_AND_PECK);
    wpa_sm_set_assoc_rsnxe(sm, 0, 0);
    if (wpa_sm_set_assoc_rsnxe_default(sm, rsnx, rsnx_len) < 0) {
        wpa_printf(MSG_ERROR, "supp: no RSNX element for the association");
        return -1;
    }
    /* hostap keeps the element of an earlier association unless told to drop it, and would send that in message 2/4. */
    wpa_sm_set_assoc_wpa_ie(sm, 0, 0);
    if (wpa_sm_set_assoc_wpa_ie_default(sm, rsn, rsn_len) < 0) {
        wpa_printf(MSG_ERROR, "supp: no RSN element for the association");
        return -1;
    }
    wpa_printf(MSG_DEBUG, "supp: to " MACSTR " with AKM 0x%x, pairwise 0x%x, PMF %d", MAC2STR(n->bssid), n->key_mgmt,
               n->pairwise, n->pmf);
    return 0;
}

void supp_associated(const uint8_t *bssid)
{
    os_memcpy(supp.bssid, bssid, ETH_ALEN);
    wpa_sm_notify_assoc(supp.sm, supp.bssid);
}

void supp_disassociated(void)
{
    sae_free();
    wpa_sm_notify_disassoc(supp.sm);
}

int supp_rx_eapol(const uint8_t *src, const uint8_t *buf, size_t len)
{
    return wpa_sm_rx_eapol(supp.sm, src, buf, len, FRAME_ENCRYPTION_UNKNOWN);
}

int supp_in_4way(void)
{
    return supp.state == WPA_4WAY_HANDSHAKE;
}

void supp_michael_failed(int pairwise)
{
    wpa_sm_key_request(supp.sm, 1, pairwise);
}

/* hostap's own timeout, in the supplicant's thread: ask the access point for a new pairwise key. */
static void rekey_timeout(void *eloop_data, void *user_data)
{
    (void)eloop_data, (void)user_data;
    wpa_sm_key_request(supp.sm, 0, 1);
}

void supp_rekey(int after_s)
{
    if (eloop_register_timeout((unsigned)after_s, 0, rekey_timeout, 0, 0) < 0) {
        wpa_printf(MSG_ERROR, "supp: no room to ask for a pairwise rekey");
    }
}

void supp_run(int (*fn)(void *arg), void *arg)
{
    if (supp.link) {
        supp.link->run(fn, arg);
    }
}
