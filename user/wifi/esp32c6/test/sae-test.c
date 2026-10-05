/*
 * WPA3's SAE on the host, as the ESP32-C6's driver runs it: hostap's sae.c and dragonfly.c over ec.c and Mbed TLS.
 * IEEE 802.11's test vectors, by hunting and pecking and by hash to element,
 * and whole exchanges between a station and an access point, with the same password and with another.
 */

#include "utils/includes.h"

#include "utils/common.h"

#include "common/defs.h"
#include "common/sae.h"
#include "crypto/crypto.h"
#include "utils/wpabuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Mbed TLS's allocator, as mbedtls/config.h names it: the driver's heap on the chip. */
void *osi_calloc(size_t n, size_t size)
{
    return calloc(n, size);
}

void osi_free(void *p)
{
    free(p);
}

static int failures;

static void check(int ok, const char *what)
{
    if (!ok) {
        printf("sae-test: FAIL %s\n", what);
        failures++;
    }
}

/*
 * What ec.c does itself rather than leave to Mbed TLS, and SAE's vectors below do not reach:
 * a point off the curve refused, the point at infinity, a scalar reduced by the order.
 */
static void edges(void)
{
    static const u8 g[64] = { /* P-256's generator, x then y */
        0x6b, 0x17, 0xd1, 0xf2, 0xe1, 0x2c, 0x42, 0x47, 0xf8, 0xbc, 0xe6, 0xe5, 0x63, 0xa4, 0x40, 0xf2,
        0x77, 0x03, 0x7d, 0x81, 0x2d, 0xeb, 0x33, 0xa0, 0xf4, 0xa1, 0x39, 0x45, 0xd8, 0x98, 0xc2, 0x96,
        0x4f, 0xe3, 0x42, 0xe2, 0xfe, 0x1a, 0x7f, 0x9b, 0x8e, 0xe7, 0xeb, 0x4a, 0x7c, 0x0f, 0x9e, 0x16,
        0x2b, 0xce, 0x33, 0x57, 0x6b, 0x31, 0x5e, 0xce, 0xcb, 0xb6, 0x40, 0x68, 0x37, 0xbf, 0x51, 0xf5,
    };
    u8 off[64], x[32];
    memcpy(off, g, sizeof(off));
    off[63] ^= 1;
    struct crypto_ec *e = crypto_ec_init(19);
    struct crypto_ec_point *gp = crypto_ec_point_from_bin(e, g), *neg = crypto_ec_point_from_bin(e, g);
    struct crypto_ec_point *g2 = crypto_ec_point_init(e), *r = crypto_ec_point_init(e), *inf = crypto_ec_point_init(e);
    struct crypto_bignum *two = crypto_bignum_init_uint(2), *k = crypto_bignum_init();
    const struct crypto_bignum *n = crypto_ec_get_order(e);

    check(gp && neg && crypto_ec_point_from_bin(e, off) == 0, "a point off the curve refused");
    check(crypto_ec_point_add(e, gp, gp, g2) == 0 && crypto_ec_point_mul(e, gp, two, r) == 0 &&
              crypto_ec_point_cmp(e, g2, r) == 0,
          "G + G = 2G");
    check(crypto_ec_point_add(e, g2, inf, r) == 0 && crypto_ec_point_cmp(e, g2, r) == 0 &&
              crypto_ec_point_mul(e, inf, two, r) == 0 && crypto_ec_point_is_at_infinity(e, r),
          "the point at infinity added and multiplied");
    check(crypto_ec_point_mul(e, gp, n, r) == 0 && crypto_ec_point_is_at_infinity(e, r), "nG at infinity");
    check(crypto_bignum_add(n, two, k) == 0 && crypto_ec_point_mul(e, gp, k, r) == 0 && crypto_ec_point_cmp(e, g2, r) == 0,
          "(n + 2)G = 2G");
    check(crypto_ec_point_invert(e, neg) == 0 && crypto_ec_point_add(e, gp, neg, r) == 0 &&
              crypto_ec_point_is_at_infinity(e, r) && crypto_ec_point_to_bin(e, r, x, 0) < 0,
          "G - G at infinity, with no coordinates");

    crypto_bignum_deinit(two, 1);
    crypto_bignum_deinit(k, 1);
    struct crypto_ec_point *points[] = { gp, neg, g2, r, inf };
    for (unsigned i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
        crypto_ec_point_deinit(points[i], 1);
    }
    crypto_ec_deinit(e);
}

/* --- IEEE Std 802.11-2020, Annex J.10, which hostap's common_module_tests.c checks on its backends. --- */

static const u8 addr1[ETH_ALEN] = { 0x4d, 0x3f, 0x2f, 0xff, 0xe3, 0x87 };
static const u8 addr2[ETH_ALEN] = { 0xa5, 0xd8, 0xaa, 0x95, 0x8e, 0x3c };
static const char ssid[] = "byteme";
static const char pw[] = "mekmitasdigoat";
static const char pwid[] = "psk4internet";

static void vectors(void)
{
    static const u8 local_rand[] = {
        0x99, 0x24, 0x65, 0xfd, 0x3d, 0xaa, 0x3c, 0x60, 0xaa, 0x65, 0x65, 0xb7, 0xf6, 0x2a, 0x2a, 0x7f,
        0x2e, 0x12, 0xdd, 0x12, 0xf1, 0x98, 0xfa, 0xf4, 0xfb, 0xed, 0x89, 0xd7, 0xff, 0x1a, 0xce, 0x94,
    };
    static const u8 local_mask[] = {
        0x95, 0x07, 0xa9, 0x0f, 0x77, 0x7a, 0x04, 0x4d, 0x6a, 0x08, 0x30, 0xb9, 0x1e, 0xa3, 0xd5, 0xdd,
        0x70, 0xbe, 0xce, 0x44, 0xe1, 0xac, 0xff, 0xb8, 0x69, 0x83, 0xb5, 0xe1, 0xbf, 0x9f, 0xb3, 0x22,
    };
    static const u8 local_commit[] = {
        0x13, 0x00, 0x2e, 0x2c, 0x0f, 0x0d, 0xb5, 0x24, 0x40, 0xad, 0x14, 0x6d, 0x96, 0x71, 0x14, 0xce,
        0x00, 0x5c, 0xe1, 0xea, 0xb0, 0xaa, 0x2c, 0x2e, 0x5c, 0x28, 0x71, 0xb7, 0x74, 0xf6, 0xc2, 0x57,
        0x5c, 0x65, 0xd5, 0xad, 0x9e, 0x00, 0x82, 0x97, 0x07, 0xaa, 0x36, 0xba, 0x8b, 0x85, 0x97, 0x38,
        0xfc, 0x96, 0x1d, 0x08, 0x24, 0x35, 0x05, 0xf4, 0x7c, 0x03, 0x53, 0x76, 0xd7, 0xac, 0x4b, 0xc8,
        0xd7, 0xb9, 0x50, 0x83, 0xbf, 0x43, 0x82, 0x7d, 0x0f, 0xc3, 0x1e, 0xd7, 0x78, 0xdd, 0x36, 0x71,
        0xfd, 0x21, 0xa4, 0x6d, 0x10, 0x91, 0xd6, 0x4b, 0x6f, 0x9a, 0x1e, 0x12, 0x72, 0x62, 0x13, 0x25,
        0xdb, 0xe1,
    };
    static const u8 peer_commit[] = {
        0x13, 0x00, 0x59, 0x1b, 0x96, 0xf3, 0x39, 0x7f, 0xb9, 0x45, 0x10, 0x08, 0x48, 0xe7, 0xb5, 0x50,
        0x54, 0x3b, 0x67, 0x20, 0xd8, 0x83, 0x37, 0xee, 0x93, 0xfc, 0x49, 0xfd, 0x6d, 0xf7, 0xe0, 0x8b,
        0x52, 0x23, 0xe7, 0x1b, 0x9b, 0xb0, 0x48, 0xd3, 0x87, 0x3f, 0x20, 0x55, 0x69, 0x53, 0xa9, 0x6c,
        0x91, 0x53, 0x6f, 0xd8, 0xee, 0x6c, 0xa9, 0xb4, 0xa6, 0x8a, 0x14, 0x8b, 0x05, 0x6a, 0x90, 0x9b,
        0xe0, 0x3e, 0x83, 0xae, 0x20, 0x8f, 0x60, 0xf8, 0xef, 0x55, 0x37, 0x85, 0x80, 0x74, 0xdb, 0x06,
        0x68, 0x70, 0x32, 0x39, 0x98, 0x62, 0x99, 0x9b, 0x51, 0x1e, 0x0a, 0x15, 0x52, 0xa5, 0xfe, 0xa3,
        0x17, 0xc2,
    };
    static const u8 kck[] = {
        0x1e, 0x73, 0x3f, 0x6d, 0x9b, 0xd5, 0x32, 0x56, 0x28, 0x73, 0x04, 0x33, 0x88, 0x31, 0xb0, 0x9a,
        0x39, 0x40, 0x6d, 0x12, 0x10, 0x17, 0x07, 0x3a, 0x5c, 0x30, 0xdb, 0x36, 0xf3, 0x6c, 0xb8, 0x1a,
    };
    static const u8 pmk[] = {
        0x4e, 0x4d, 0xfa, 0xb1, 0xa2, 0xdd, 0x8a, 0xc1, 0xa9, 0x17, 0x90, 0xf9, 0x53, 0xfa, 0xaa, 0x45,
        0x2a, 0xe5, 0xc6, 0x87, 0x3a, 0xb7, 0x5b, 0x63, 0x60, 0x5b, 0xa6, 0x63, 0xf8, 0xa7, 0xfe, 0x59,
    };
    static const u8 pmkid[] = {
        0x87, 0x47, 0xa6, 0x00, 0xee, 0xa3, 0xf9, 0xf2, 0x24, 0x75, 0xdf, 0x58, 0xca, 0x1e, 0x54, 0x98,
    };
    static const u8 pwe_x[] = {
        0xc9, 0x30, 0x49, 0xb9, 0xe6, 0x40, 0x00, 0xf8, 0x48, 0x20, 0x16, 0x49, 0xe9, 0x99, 0xf2, 0xb5,
        0xc2, 0x2d, 0xea, 0x69, 0xb5, 0x63, 0x2c, 0x9d, 0xf4, 0xd6, 0x33, 0xb8, 0xaa, 0x1f, 0x6c, 0x1e,
    };
    static const u8 pwe_y[] = {
        0x73, 0x63, 0x4e, 0x94, 0xb5, 0x3d, 0x82, 0xe7, 0x38, 0x3a, 0x8d, 0x25, 0x81, 0x99, 0xd9, 0xdc,
        0x1a, 0x5e, 0xe8, 0x26, 0x9d, 0x06, 0x03, 0x82, 0xcc, 0xbf, 0x33, 0xe6, 0x14, 0xff, 0x59, 0xa0,
    };
    static const u8 addr1b[ETH_ALEN] = { 0x00, 0x09, 0x5b, 0x66, 0xec, 0x1e };
    static const u8 addr2b[ETH_ALEN] = { 0x00, 0x0b, 0x6b, 0xd9, 0x02, 0x46 };

    /* Hunting and pecking: the commit with the vector's rand and mask, then the keys of the peer's commit. */
    struct sae_data sae;
    memset(&sae, 0, sizeof(sae));
    struct wpabuf *buf = wpabuf_alloc(1000);
    struct crypto_bignum *mask = crypto_bignum_init_set(local_mask, sizeof(local_mask));
    int ok = buf && mask && sae_set_group(&sae, 19) == 0 &&
             sae_prepare_commit(addr1, addr2, (const u8 *)pw, strlen(pw), &sae) == 0;
    check(ok, "a commit prepared");
    if (ok) {
        crypto_bignum_deinit(sae.tmp->sae_rand, 1);
        sae.tmp->sae_rand = crypto_bignum_init_set(local_rand, sizeof(local_rand));
        ok = sae.tmp->sae_rand && crypto_bignum_add(sae.tmp->sae_rand, mask, sae.tmp->own_commit_scalar) == 0 &&
             crypto_bignum_mod(sae.tmp->own_commit_scalar, sae.tmp->order, sae.tmp->own_commit_scalar) == 0 &&
             crypto_ec_point_mul(sae.tmp->ec, sae.tmp->pwe_ecc, mask, sae.tmp->own_commit_element_ecc) == 0 &&
             crypto_ec_point_invert(sae.tmp->ec, sae.tmp->own_commit_element_ecc) == 0 &&
             sae_write_commit(&sae, buf, 0, 0, 0) == 0;
        check(ok && wpabuf_len(buf) == sizeof(local_commit) &&
                  memcmp(wpabuf_head(buf), local_commit, sizeof(local_commit)) == 0,
              "J.10's commit");
        ok = sae_parse_commit(&sae, peer_commit, sizeof(peer_commit), 0, 0, 0, 0, 0) == 0 &&
             sae_process_commit(&sae) == 0;
        check(ok && memcmp(kck, sae.tmp->kck, SAE_KCK_LEN) == 0, "J.10's KCK");
        check(ok && memcmp(pmk, sae.pmk, SAE_PMK_LEN) == 0, "J.10's PMK");
        check(ok && memcmp(pmkid, sae.pmkid, SAE_PMKID_LEN) == 0, "J.10's PMKID");
    }
    sae_clear_data(&sae);
    wpabuf_free(buf);
    crypto_bignum_deinit(mask, 1);

    /* Hash to element: the password's PT, and the PWE of two addresses from it. */
    int groups[] = { 19, 0 };
    struct sae_pt *pt = sae_derive_pt(groups, (const u8 *)ssid, strlen(ssid), (const u8 *)pw, strlen(pw),
                                      (const u8 *)pwid, strlen(pwid));
    check(pt != 0 && pt->group == 19, "J.10's PT");
    if (pt) {
        struct crypto_ec_point *pwe = sae_derive_pwe_from_pt_ecc(pt, addr1b, addr2b);
        u8 bin[64];
        check(pwe && crypto_ec_point_to_bin(pt->ec, pwe, bin, bin + 32) == 0 && memcmp(bin, pwe_x, 32) == 0 &&
                  memcmp(bin + 32, pwe_y, 32) == 0,
              "J.10's PWE from its PT");
        crypto_ec_point_deinit(pwe, 1);
        sae_deinit_pt(pt);
    }
}

/* --- Whole exchanges, as a station and an access point run them. --- */

static const u8 sta_addr[ETH_ALEN] = { 0x10, 0xbd, 0xa3, 0xa0, 0x2d, 0xd0 };
static const u8 ap_addr[ETH_ALEN] = { 0xf4, 0x1e, 0x57, 0x2d, 0xc0, 0x70 };

struct side {
    struct sae_data sae;
    struct sae_pt *pt;
    struct wpabuf *commit, *confirm;
};

static int prepare(struct side *s, int h2e, const char *password, const u8 *own, const u8 *peer)
{
    int groups[] = { 19, 0 };
    memset(s, 0, sizeof(*s));
    s->commit = wpabuf_alloc(SAE_COMMIT_MAX_LEN);
    s->confirm = wpabuf_alloc(SAE_CONFIRM_MAX_LEN);
    if (!s->commit || !s->confirm || sae_set_group(&s->sae, 19) < 0) {
        return -1;
    }
    s->sae.akmp = WPA_KEY_MGMT_SAE;
    if (h2e) {
        s->pt = sae_derive_pt(groups, (const u8 *)ssid, strlen(ssid), (const u8 *)password, strlen(password), 0, 0);
        if (!s->pt || sae_prepare_commit_pt(&s->sae, s->pt, own, peer, 0, 0) < 0) {
            return -1;
        }
    } else if (sae_prepare_commit(own, peer, (const u8 *)password, strlen(password), &s->sae) < 0) {
        return -1;
    }
    return sae_write_commit(&s->sae, s->commit, 0, 0, 0);
}

static int take_commit(struct side *s, const struct side *peer, int h2e)
{
    int groups[] = { 19, 0 };
    if (sae_parse_commit(&s->sae, wpabuf_head(peer->commit), wpabuf_len(peer->commit), 0, 0, groups, h2e, 0) != 0 ||
        sae_process_commit(&s->sae) < 0) {
        return -1;
    }
    return sae_write_confirm(&s->sae, s->confirm);
}

static void release(struct side *s)
{
    sae_clear_data(&s->sae);
    sae_deinit_pt(s->pt);
    wpabuf_free(s->commit);
    wpabuf_free(s->confirm);
}

static void exchange(int h2e, const char *sta_pw, const char *ap_pw)
{
    struct side sta, ap;
    char what[80];
    int same = strcmp(sta_pw, ap_pw) == 0;
    snprintf(what, sizeof(what), "%s, %s password", h2e ? "hash to element" : "hunting and pecking",
             same ? "the same" : "another");
    int ok = prepare(&sta, h2e, sta_pw, sta_addr, ap_addr) == 0 && prepare(&ap, h2e, ap_pw, ap_addr, sta_addr) == 0 &&
             take_commit(&ap, &sta, h2e) == 0 && take_commit(&sta, &ap, h2e) == 0;
    check(ok, what);
    if (ok) {
        int sta_ok = sae_check_confirm(&sta.sae, wpabuf_head(ap.confirm), wpabuf_len(ap.confirm), 0) == 0;
        int ap_ok = sae_check_confirm(&ap.sae, wpabuf_head(sta.confirm), wpabuf_len(sta.confirm), 0) == 0;
        if (same) {
            check(sta_ok && ap_ok && sta.sae.pmk_len == SAE_PMK_LEN &&
                      memcmp(sta.sae.pmk, ap.sae.pmk, SAE_PMK_LEN) == 0 &&
                      memcmp(sta.sae.pmkid, ap.sae.pmkid, SAE_PMKID_LEN) == 0,
                  what);
        } else {
            check(!sta_ok && !ap_ok, what);
        }
    }
    release(&sta);
    release(&ap);
}

int main(void)
{
    /* A curve's arithmetic gone wrong can leave SAE's loops looking for a number that never comes. */
    alarm(60);
    edges();
    vectors();
    for (int h2e = 0; h2e < 2; h2e++) {
        exchange(h2e, "correct horse battery staple", "correct horse battery staple");
        exchange(h2e, "correct horse battery staple", "correct horse battery stapler");
    }
    if (failures) {
        printf("sae-test: %d failed\n", failures);
        return 1;
    }
    printf("sae-test: ok\n");
    return 0;
}
