#ifndef RVUOS_WIFI_KEYS_H
#define RVUOS_WIFI_KEYS_H

/*
 * The station's pairwise key: the one in use and the one a 4-way handshake stages beside it.
 * hostap installs the key it derives for receiving alone before the 4/4 and for sending too after,
 * so the station sends and receives under different keys for the moment in between; sta.c carries out
 * what this decides, and keeps no transition of its own.
 */

#include <stdint.h>

#include "ccmp.h"

/* A pairwise key as an install names it: the handshake's id on the frame, and its bytes. */
struct key_value {
    uint8_t id;
    uint8_t tk[CCMP_TK_LEN];
};

/* The station's pairwise key: the one in use, and the one staged for receiving until an install may send. */
struct keys {
    int cur_set, next_set;
    struct key_value cur, next;
};

/*
 * What the station does with a pairwise key an install brings. An install that neither sends nor is staged,
 * the pairwise key for receiving alone of extended key ID, is refused; the station negotiates none of that.
 */
enum key_action {
    KEY_REFUSE,  /* an install the station does not make */
    KEY_AGAIN,   /* the key already in use, or already staged: its counters stay */
    KEY_STAGE,   /* kept for receiving until the install that may send; the key in use still sends */
    KEY_PROMOTE, /* the staged key takes over, for receiving and for sending */
    KEY_INSTALL, /* a new key in use at once, for receiving and for sending */
};

/*
 * The action for the install in, with ks updated: the key in use, one staged, or either moved by the action.
 * tx when the install may send with, next_flag when it is the handshake's staged one. The caller has already
 * refused what it does not carry out, and seeds the counters and the MAC entry the action leaves it.
 */
enum key_action keys_apply(struct keys *ks, const struct key_value *in, int tx, int next_flag);

#endif
