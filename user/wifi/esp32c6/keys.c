/* The station's keys' transitions; see keys.h. */

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

int keys_group_slot(uint32_t first, uint32_t id)
{
    return id >= first && id - first < KEYS_GROUP ? (int)(id - first) : -1;
}

enum key_action keys_group_apply(struct group_keys *g, uint32_t first, const struct key_value *in)
{
    int s = keys_group_slot(first, in->id);
    if (s < 0) {
        return KEY_REFUSE;
    }
    if (g->set[s] && same(&g->k[s], in)) {
        return KEY_AGAIN;
    }
    g->k[s] = *in;
    g->set[s] = 1;
    return KEY_INSTALL;
}
