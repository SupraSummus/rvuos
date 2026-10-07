#ifndef RVUOS_WIFI_SUPP_H
#define RVUOS_WIFI_SUPP_H

/*
 * The supplicant: hostap's, upstream's own (src/rsn_supp/ of wpa_supplicant, BSD), not ESP-IDF's fork of it,
 * for a station that gives it a link: Espressif's libraries, see wpa.c, or, to come, the driver's own; see TODO.md.
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
 * WPA2's personal by PSK, CCMP pairwise and for the group, and management frames unprotected.
 * 0, or -1 if the access point offers none of them, or requires protection, which the log says.
 */
int supp_choose(struct supp_network *n);

/* The PMK of a passphrase, or a PSK, and an SSID, derived ahead, which takes a fifth of a second. */
void supp_prepare(const char *ssid, const char *pass);

/*
 * The station is to associate with n: the RSN element to associate with into rsn, at most *rsn_len bytes,
 * and the RSNX element into rsnx, at most *rsnx_len, of length 0 if none; 0, or -1 if the supplicant cannot.
 */
int supp_connect(const struct supp_network *n, uint8_t *rsn, size_t *rsn_len, uint8_t *rsnx, size_t *rsnx_len);

/* The access point at bssid answered the association, which starts the handshake. */
void supp_associated(const uint8_t *bssid);

/* The station left the access point, or was let go. */
void supp_disassociated(void);

/* An EAPOL frame from src, from its 802.1X header on; what hostap's wpa_sm_rx_eapol answers. */
int supp_rx_eapol(const uint8_t *src, const uint8_t *buf, size_t len);

/* Whether the 4-way handshake runs. */
int supp_in_4way(void);

/* Michael's check failed on a frame received, to the station if pairwise, else to the group. */
void supp_michael_failed(int pairwise);

/* fn(arg) run in the supplicant's thread, through the link; see hostap.c. */
void supp_run(int (*fn)(void *arg), void *arg);

/*
 * WPA3's SAE with the network supp_connect was given.
 * A message to send, the commit to bssid, or once the access point asked for a token, the same commit again with it,
 * then the confirm: the message, which lasts until the next, its length into *len; or 0 if none.
 */
const uint8_t *supp_sae_commit(const uint8_t *bssid, size_t *len);
const uint8_t *supp_sae_confirm(size_t *len);

/*
 * A message received, from its first field on: the access point's commit, with its frame's status, which may ask
 * for a token instead, or its confirm. SUPP_TAKEN, SUPP_DISCARD if it is to be dropped silently, SUPP_FAILED,
 * or IEEE 802.11's status to refuse it with.
 */
#define SUPP_TAKEN   0
#define SUPP_FAILED  (-1)
#define SUPP_DISCARD (-2)
int supp_sae_take_commit(const uint8_t *buf, size_t len, uint16_t status);
int supp_sae_take_confirm(const uint8_t *buf, size_t len);

#endif
