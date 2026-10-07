/*
 * keys.c on the host, under the sanitizers.
 * The transitions a 4-way handshake drives, as hostap's wpa_supplicant_process_3_of_4 does:
 * a first join stages the derived key before the 4/4 and promotes it after, a rekey stages the new key while
 * the old one still sends, and a message 3 that comes again after the promotion is neither staged nor promoted,
 * or its sending packet numbers would begin again; that last is the KRACK path. Extended key ID is refused.
 * And the group's: a rekey keeps the old key beside the new, and a key installed again keeps its counters.
 */

#include <stdio.h>
#include <string.h>

#include "keys.h"

static int failures;

static void check(int ok, const char *what)
{
    if (!ok) {
        printf("keys-test: FAIL %s\n", what);
        failures++;
    }
}

/* A key as the handshake derives it: without extended key ID the pairwise key's id is 0, so the bytes tell them apart. */
static struct key_value key(uint8_t tag)
{
    struct key_value k;
    memset(&k, 0, sizeof(k));
    memset(k.tk, tag, CCMP_TK_LEN);
    return k;
}

static void first_join(void)
{
    struct keys s = { 0 };
    struct key_value k1 = key(0x11);
    check(keys_apply(&s, &k1, 0, 1) == KEY_STAGE, "the first join stages the key before the 4/4");
    check(!s.cur_set && s.next_set, "with nothing in use while it is staged");
    check(keys_apply(&s, &k1, 1, 0) == KEY_PROMOTE, "the 4/4's answer promotes it");
    check(s.cur_set && !s.next_set && memcmp(s.cur.tk, k1.tk, CCMP_TK_LEN) == 0, "to the key in use alone");
}

static void rekey(void)
{
    struct keys s = { 0 };
    struct key_value k1 = key(0x11), k2 = key(0x22);
    keys_apply(&s, &k1, 0, 1);
    keys_apply(&s, &k1, 1, 0);
    check(keys_apply(&s, &k2, 0, 1) == KEY_STAGE, "a rekey stages the new key");
    check(s.cur_set && memcmp(s.cur.tk, k1.tk, CCMP_TK_LEN) == 0, "the old key still sends while the new one is staged");
    check(keys_apply(&s, &k2, 1, 0) == KEY_PROMOTE, "and the 4/4's answer promotes it");
    check(s.cur_set && !s.next_set && memcmp(s.cur.tk, k2.tk, CCMP_TK_LEN) == 0, "to the new key in use");
}

/* A message 3 that comes again after the promotion: the same key, so neither staged nor promoted. */
static void again(void)
{
    struct keys s = { 0 };
    struct key_value k1 = key(0x11);
    keys_apply(&s, &k1, 0, 1);
    keys_apply(&s, &k1, 1, 0);
    check(keys_apply(&s, &k1, 0, 1) == KEY_AGAIN, "the key in use staged again keeps it");
    check(keys_apply(&s, &k1, 1, 0) == KEY_AGAIN, "and installed again is no promotion");
    check(s.cur_set && !s.next_set && memcmp(s.cur.tk, k1.tk, CCMP_TK_LEN) == 0, "with the key in use unchanged");
}

static void fresh_and_refused(void)
{
    struct keys s = { 0 };
    struct key_value k1 = key(0x11), k2 = key(0x22), k3 = key(0x33);
    check(keys_apply(&s, &k1, 1, 0) == KEY_INSTALL, "a key that may send at once installs");
    check(keys_apply(&s, &k2, 0, 1) == KEY_STAGE, "the next key stages");
    check(keys_apply(&s, &k3, 1, 0) == KEY_INSTALL, "a key neither staged nor in use installs over both");
    check(s.cur_set && !s.next_set && memcmp(s.cur.tk, k3.tk, CCMP_TK_LEN) == 0, "to the key in use alone");
    check(keys_apply(&s, &k3, 0, 0) == KEY_REFUSE, "the pairwise key for receiving alone, of extended key ID, is refused");
    check(s.cur_set && memcmp(s.cur.tk, k3.tk, CCMP_TK_LEN) == 0, "which leaves the key in use alone");
}

/* A group key of id, its bytes told apart by tag. */
static struct key_value group(uint8_t id, uint8_t tag)
{
    struct key_value k = key(tag);
    k.id = id;
    return k;
}

static int holds(const struct group_keys *g, int slot, const struct key_value *k)
{
    return g->set[slot] && g->k[slot].id == k->id && memcmp(g->k[slot].tk, k->tk, CCMP_TK_LEN) == 0;
}

/* A group rekey: the access point's new key under the other id, while it still sends under the old one. */
static void group_rekey(void)
{
    struct group_keys g = { 0 };
    struct key_value a = group(2, 0x11), b = group(1, 0x22), c = group(2, 0x33);
    check(keys_group_apply(&g, KEYS_GROUP_ID, &a) == KEY_INSTALL && holds(&g, 1, &a) && !g.set[0],
          "the join's group key takes its id's slot");
    check(keys_group_apply(&g, KEYS_GROUP_ID, &b) == KEY_INSTALL && holds(&g, 0, &b) && holds(&g, 1, &a),
          "a rekey's key, of the other id, goes beside the old one");
    check(keys_group_apply(&g, KEYS_GROUP_ID, &a) == KEY_AGAIN,
          "the old key installed again keeps its counters, as KRACK's group path asks");
    check(keys_group_apply(&g, KEYS_GROUP_ID, &c) == KEY_INSTALL && holds(&g, 1, &c) && holds(&g, 0, &b),
          "the next rekey takes the old key's slot");
}

/* Ids an access point does not alternate have no slot; the management group key's are 4 and 5. */
static void group_ids(void)
{
    struct group_keys g = { 0 }, m = { 0 };
    struct key_value zero = group(0, 0x11), three = group(3, 0x22), four = group(4, 0x33);
    check(keys_group_apply(&g, KEYS_GROUP_ID, &zero) == KEY_REFUSE &&
              keys_group_apply(&g, KEYS_GROUP_ID, &three) == KEY_REFUSE && !g.set[0] && !g.set[1],
          "a group key of id 0 or 3 is refused");
    check(keys_group_apply(&m, KEYS_MGMT_GROUP_ID, &four) == KEY_INSTALL && holds(&m, 0, &four),
          "a management group key of id 4 takes the first slot");
}

int main(void)
{
    first_join();
    rekey();
    again();
    fresh_and_refused();
    group_rekey();
    group_ids();
    printf("keys-test: %s\n", failures ? "FAILED" : "ok");
    return failures != 0;
}
