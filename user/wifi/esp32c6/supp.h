#ifndef RVUOS_WIFI_SUPP_H
#define RVUOS_WIFI_SUPP_H

/*
 * The supplicant: hostap's, upstream's own (src/rsn_supp/ of wpa_supplicant, BSD), not ESP-IDF's fork of it,
 * for the driver's own station, sta.c, which gives it a link.
 * The station names the network it chose, and the supplicant gives it the RSN and RSNX elements to associate with;
 * then it runs the 4-way and group key handshakes over the EAPOL frames the station hands it,
 * and sends its own and installs the keys through the link.
 * WPA3's personal authenticates with SAE before the association, whose messages the supplicant makes and checks,
 * on P-256 alone, see ec.c, by hash to element where the access point offers it, else by hunting and pecking.
 * It runs in one thread, where the link's run runs eloop's timeouts too; see hostap.c.
 */

#include <stddef.h>
#include <stdint.h>

/* EAPOL's Ethernet type, the proto of the frames the supplicant sends. */
#define SUPP_EAPOL 0x888eu

/* hostap's enum wpa_alg, as far as the station names it; see common/defs.h. */
#define SUPP_ALG_NONE 0
#define SUPP_ALG_CCMP 3
#define SUPP_ALG_BIP_CMAC_128 4

/* hostap's WPA_KEY_MGMT_SAE, which the station tells WPA3's personal by; see common/defs.h. */
#define SUPP_KEY_MGMT_SAE 0x400u

/* hostap's enum key_flag bits the station names; see common/defs.h. */
#define SUPP_KEY_GROUP 0x10  /* a key is the group's rather than the pairwise one */
#define SUPP_KEY_TX 0x08     /* a key may send with, not only receive with */
#define SUPP_KEY_MODIFY 0x01 /* an install only modifies a key: its activate for sending */
#define SUPP_KEY_NEXT 0x80   /* a key is staged for receiving until one that may send; see keys.h */

/* A key the handshakes derived, as hostap's driver is handed it. */
struct supp_key {
    int alg;              /* hostap's enum wpa_alg */
    const uint8_t *addr;  /* the peer's; for a group key the broadcast address or 0 */
    int idx, set_tx;
    const uint8_t *seq;   /* the counter of frames received to start from, little-endian, or 0 */
    size_t seq_len;
    const uint8_t *key;
    size_t key_len;
    int flag;             /* hostap's enum key_flag */
};

/* What the supplicant asks of the station it runs for. */
struct supp_link {
    /* A frame of Ethernet type proto, an EAPOL frame, to dest from the station; 0, or -1 if not sent. */
    int (*send)(const uint8_t *dest, uint16_t proto, const uint8_t *buf, size_t len);
    /* A key installed, or with hostap's WPA_ALG_NONE removed; 0, or -1 if not. */
    int (*set_key)(const struct supp_key *k);
    /* The handshakes are done: the station sends and receives with the keys from here on. */
    void (*completed)(void);
    /* The station leaves the access point, for reason, IEEE 802.11's. */
    void (*deauthenticate)(uint16_t reason);
    /* fn(arg) run in the supplicant's thread, soon; eloop's timeouts come so. */
    void (*run)(int (*fn)(void *arg), void *arg);
};

/* The network the station chose, to associate with. */
struct supp_network {
    const uint8_t *bssid;
    const uint8_t *ssid;
    size_t ssid_len;
    const char *pass;      /* 8 to 63 characters, or the PSK in 64 hexadecimal digits */
    int key_mgmt;          /* hostap's WPA_KEY_MGMT_: PSK, PSK_SHA256 or SAE */
    int pairwise, group;   /* hostap's WPA_CIPHER_ */
    int pmf;               /* management frames protected, which SAE needs, if the access point takes it */
    int pmf_ok;            /* whether the station may protect them, from its configuration */
    int sae_ok;            /* whether the station may authenticate by SAE, from its configuration */
    int mgmt_group;        /* with pmf, the management group cipher, hostap's WPA_CIPHER_ of a BIP */
    int sae_pwe;           /* the password elements the station takes, hostap's SAE_PWE_ */
    const uint8_t *ap_rsn; /* the access point's RSN element, whole, or 0 */
    const uint8_t *ap_rsnx; /* its RSNX element, or 0 */
};

/* The supplicant bound to the station at own, through link, which lasts: made at the first call; 0, or -1. */
int supp_init(const struct supp_link *link, const uint8_t *own);
void supp_deinit(void);

/*
 * The suites the driver's own station takes of those the access point offers in n->ap_rsn, into n:
 * WPA3's personal by SAE where the access point offers it and n->sae_ok and n->pmf_ok allow, since SAE needs
 * management frames protected, else WPA2's personal by PSK; CCMP pairwise and for the group;
 * and management frames protected (PMF) where the access point offers them and n->pmf_ok allows.
 * SAE's password element by hash to element where the access point offers it, else by hunting and pecking.
 * 0, or -1 if the access point offers none of them, or requires protection the station is not to give,
 * which the log says.
 */
int supp_choose(struct supp_network *n);

/*
 * The station is to associate with n: the RSN element to associate with into rsn, at most *rsn_len bytes,
 * and the RSNX element into rsnx, at most *rsnx_len, of length 0 if none; 0, or -1 if the supplicant cannot.
 */
int supp_connect(const struct supp_network *n, uint8_t *rsn, size_t *rsn_len, uint8_t *rsnx, size_t *rsnx_len);

/* The access point at bssid answered the association, which starts the handshake. */
void supp_associated(const uint8_t *bssid);

/* The station left the access point, or was let go. */
void supp_disassociated(void);

/* Whether an EAPOL frame was protected, which hostap uses under PMF. */
#define SUPP_EAPOL_CLEAR     0
#define SUPP_EAPOL_PROTECTED 1

/* An EAPOL frame from src, from its 802.1X header on, its protection encryption; what hostap's wpa_sm_rx_eapol answers. */
int supp_rx_eapol(const uint8_t *src, const uint8_t *buf, size_t len, int encryption);

/* fn(arg) run in the supplicant's thread, through the link; see hostap.c. */
void supp_run(int (*fn)(void *arg), void *arg);

/* Ask the access point for a new pairwise key, after_s seconds from now; the station's `debug=rekey` drives this. */
void supp_rekey(int after_s);

/*
 * WPA3's SAE with the network supp_connect was given.
 * A message to send, the commit to bssid, or once the access point asked for a token, the same commit again with it,
 * then the confirm, or the confirm again, its counter the next, as IEEE 802.11 sends it again:
 * the message, which lasts until the next, its length into *len; or 0 if none.
 */
const uint8_t *supp_sae_commit(const uint8_t *bssid, size_t *len);
const uint8_t *supp_sae_confirm(size_t *len);

/* IEEE 802.11's status of the authentication frame the commit goes in: hash to element's, or success. */
uint16_t supp_sae_commit_status(void);

/*
 * A message received, from its first field on: the access point's commit, with its frame's status, which may ask
 * for a token instead, or its confirm. SUPP_TAKEN, SUPP_AGAIN once a token was taken, the commit to be sent again
 * with it, SUPP_DISCARD if it is to be dropped silently, SUPP_FAILED, or IEEE 802.11's status to refuse it with.
 * A commit whose frame's status is a refusal, the access point's, fails.
 */
#define SUPP_TAKEN   0
#define SUPP_FAILED  (-1)
#define SUPP_DISCARD (-2)
#define SUPP_AGAIN   (-3)
int supp_sae_take_commit(const uint8_t *buf, size_t len, uint16_t status);
int supp_sae_take_confirm(const uint8_t *buf, size_t len);

#endif
