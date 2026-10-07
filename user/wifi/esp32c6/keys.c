/* The station's pairwise key's transitions; see keys.h. */

#include "keys.h"

#include "lib/libc.h"

/* Whether two keys are the same one: an install names a key by the id on the frame and its bytes. */
static int same(const struct key_value *a, const struct key_value *b)
{
    return a->id == b->id && memcmp(a->tk, b->tk, CCMP_TK_LEN) == 0;
}

enum key_action keys_apply(struct keys *ks, const struct key_value *in, int tx, int next_flag)
{
    if (next_flag) {
        if ((ks->cur_set && same(&ks->cur, in)) || (ks->next_set && same(&ks->next, in))) {
            return KEY_AGAIN;
        }
        ks->next = *in;
        ks->next_set = 1;
        return KEY_STAGE;
    }
    if (tx) {
        if (ks->next_set && same(&ks->next, in)) {
            ks->cur = ks->next;
            ks->cur_set = 1;
            ks->next_set = 0;
            return KEY_PROMOTE;
        }
        if (ks->cur_set && same(&ks->cur, in)) {
            return KEY_AGAIN;
        }
        ks->cur = *in;
        ks->cur_set = 1;
        ks->next_set = 0;
        return KEY_INSTALL;
    }
    return KEY_REFUSE;
}
