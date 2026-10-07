/*
 * keys.c on the host, under the sanitizers.
 * The transitions a 4-way handshake drives, as hostap's wpa_supplicant_process_3_of_4 does:
 * a first join stages the derived key before the 4/4 and promotes it after, a rekey stages the new key while
 * the old one still sends, and a message 3 that comes again after the promotion is neither staged nor promoted,
 * or its sending packet numbers would begin again; that last is the KRACK path. Extended key ID is refused.
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

int main(void)
{
    first_join();
    rekey();
    again();
    fresh_and_refused();
    printf("keys-test: %s\n", failures ? "FAILED" : "ok");
    return failures != 0;
}
