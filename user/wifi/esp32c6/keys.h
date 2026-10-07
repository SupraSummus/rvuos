#ifndef RVUOS_WIFI_KEYS_H
#define RVUOS_WIFI_KEYS_H

/*
 * The station's keys: the pairwise key in use and the one a 4-way handshake stages beside it,
 * and the group's keys, one for each of the two ids an access point alternates.
 * hostap installs the pairwise key it derives for receiving alone before the 4/4 and for sending too after,
 * so the station sends and receives under different keys for the moment in between.
 * sta.c carries out what this decides, and keeps no transition of its own.
 */

#include <stdint.h>

#include "ccmp.h"

/* A key as an install names it: the handshake's id on the frame, and its bytes. */
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
 * What the station does with a key an install brings. An install of a pairwise key that neither sends nor is staged,
 * the pairwise key for receiving alone of extended key ID, is refused; the station negotiates none of that.
 * A group key is installed or kept, never staged.
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

/*
 * A group's keys, a slot for each of the two ids its access point numbers them by: the group key's are 1 and 2,
 * and the management group key's (IGTK) 4 and 5.
 * A rekey takes the id the key in use does not have, and the access point sends under the old key until its
 * stations hold the new, so the old key stays in its slot, and a frame's key id names the slot it is read by.
 */
#define KEYS_GROUP_ID      1u /* the group key's first id */
#define KEYS_MGMT_GROUP_ID 4u /* the management group key's */
#define KEYS_GROUP         2u

struct group_keys {
    volatile int set[KEYS_GROUP]; /* written by the station's thread, read by the receiving's once it serves */
    struct key_value k[KEYS_GROUP];
};

/* The slot a key of id takes among a group's whose first id is first, or -1 for an id that has none. */
int keys_group_slot(uint32_t first, uint32_t id);

/*
 * The action for a group key an install brings, with g updated: KEY_INSTALL into the slot of its id,
 * the other slot left as it was; KEY_AGAIN for the key that slot already holds, whose counters stay;
 * KEY_REFUSE for an id without a slot.
 * The caller seeds the slot's counters, and its MAC entry, on an install.
 */
enum key_action keys_group_apply(struct group_keys *g, uint32_t first, const struct key_value *in);

#endif
