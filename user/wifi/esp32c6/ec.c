/*
 * hostap's crypto_bignum and crypto_ec of crypto.h, which its SAE runs on, on Mbed TLS, see mbedtls/config.h:
 * hostap's own crypto has no elliptic curves.
 * A crypto_bignum is an mbedtls_mpi, and a crypto_ec_point an mbedtls_ecp_point, affine as Mbed TLS returns them,
 * the point at infinity the one whose Z is zero.
 * The one curve is P-256, group 19, which WPA3's personal must offer; dh_groups_get finds no finite field's group.
 * Mbed TLS refuses some of what hostap asks: a result in place of an operand,
 * a scalar outside 1 to the order less one, and the point at infinity in a sum or a product,
 * so this file takes care of those itself.
 * test/sae-test.c checks it on the host.
 */

#include "utils/includes.h"

#include "utils/common.h"

#include "crypto/crypto.h"
#include "crypto/dh_groups.h"

#include "mbedtls/bignum.h"
#include "mbedtls/ecp.h"

/* The IANA number of P-256, the one group the curve's context takes. */
#define GROUP_P256 19

#define MPI(n)        ((mbedtls_mpi *)(n))
#define CMPI(n)       ((const mbedtls_mpi *)(n))
#define POINT(p)      ((mbedtls_ecp_point *)(p))
#define CPOINT(p)     ((const mbedtls_ecp_point *)(p))
#define X(p)          ((p)->MBEDTLS_PRIVATE(X))
#define Y(p)          ((p)->MBEDTLS_PRIVATE(Y))
#define Z(p)          ((p)->MBEDTLS_PRIVATE(Z))

struct crypto_ec {
    mbedtls_ecp_group group;
    mbedtls_mpi a; /* the curve's a, -3, which Mbed TLS leaves unset for P-256 */
};

static int rng(void *ctx, unsigned char *buf, size_t len)
{
    (void)ctx;
    return os_get_random(buf, len) < 0 ? MBEDTLS_ERR_MPI_BAD_INPUT_DATA : 0;
}

/* --- Big numbers. --- */

struct crypto_bignum *crypto_bignum_init(void)
{
    mbedtls_mpi *n = os_malloc(sizeof(*n));
    if (n) {
        mbedtls_mpi_init(n);
    }
    return (struct crypto_bignum *)n;
}

struct crypto_bignum *crypto_bignum_init_set(const u8 *buf, size_t len)
{
    struct crypto_bignum *n = crypto_bignum_init();
    if (n && mbedtls_mpi_read_binary(MPI(n), buf, len) != 0) {
        crypto_bignum_deinit(n, 1);
        return 0;
    }
    return n;
}

struct crypto_bignum *crypto_bignum_init_uint(unsigned int val)
{
    u8 be[4];
    WPA_PUT_BE32(be, val);
    return crypto_bignum_init_set(be, sizeof(be));
}

/* Mbed TLS clears what it frees, so clear asks nothing more. */
void crypto_bignum_deinit(struct crypto_bignum *n, int clear)
{
    (void)clear;
    if (n) {
        mbedtls_mpi_free(MPI(n));
        os_free(n);
    }
}

int crypto_bignum_to_bin(const struct crypto_bignum *a, u8 *buf, size_t buflen, size_t padlen)
{
    size_t n = mbedtls_mpi_size(CMPI(a));
    if (padlen > buflen || n > buflen) {
        return -1;
    }
    if (n < padlen) {
        n = padlen;
    }
    return mbedtls_mpi_write_binary(CMPI(a), buf, n) == 0 ? (int)n : -1;
}

int crypto_bignum_rand(struct crypto_bignum *r, const struct crypto_bignum *m)
{
    return mbedtls_mpi_random(MPI(r), 0, CMPI(m), rng, 0) == 0 ? 0 : -1;
}

/* The result r into d, which may be one of the operands, and r freed. */
static int into(struct crypto_bignum *d, mbedtls_mpi *r, int status)
{
    if (status == 0) {
        status = mbedtls_mpi_copy(MPI(d), r);
    }
    mbedtls_mpi_free(r);
    return status == 0 ? 0 : -1;
}

/* The same of r mod m. */
static int mod_into(struct crypto_bignum *d, mbedtls_mpi *r, const struct crypto_bignum *m, int status)
{
    mbedtls_mpi s;
    mbedtls_mpi_init(&s);
    if (status == 0) {
        status = mbedtls_mpi_mod_mpi(&s, r, CMPI(m));
    }
    mbedtls_mpi_free(r);
    return into(d, &s, status);
}

int crypto_bignum_add(const struct crypto_bignum *a, const struct crypto_bignum *b, struct crypto_bignum *c)
{
    mbedtls_mpi r;
    mbedtls_mpi_init(&r);
    return into(c, &r, mbedtls_mpi_add_mpi(&r, CMPI(a), CMPI(b)));
}

int crypto_bignum_sub(const struct crypto_bignum *a, const struct crypto_bignum *b, struct crypto_bignum *c)
{
    mbedtls_mpi r;
    mbedtls_mpi_init(&r);
    return into(c, &r, mbedtls_mpi_sub_mpi(&r, CMPI(a), CMPI(b)));
}

int crypto_bignum_mod(const struct crypto_bignum *a, const struct crypto_bignum *b, struct crypto_bignum *c)
{
    mbedtls_mpi r;
    mbedtls_mpi_init(&r);
    return into(c, &r, mbedtls_mpi_mod_mpi(&r, CMPI(a), CMPI(b)));
}

int crypto_bignum_div(const struct crypto_bignum *a, const struct crypto_bignum *b, struct crypto_bignum *c)
{
    mbedtls_mpi r;
    mbedtls_mpi_init(&r);
    return into(c, &r, mbedtls_mpi_div_mpi(&r, 0, CMPI(a), CMPI(b)));
}

/* Mbed TLS takes the exponent as a secret, in a time its bits do not change. */
int crypto_bignum_exptmod(const struct crypto_bignum *a, const struct crypto_bignum *b,
                          const struct crypto_bignum *c, struct crypto_bignum *d)
{
    mbedtls_mpi r;
    mbedtls_mpi_init(&r);
    return into(d, &r, mbedtls_mpi_exp_mod(&r, CMPI(a), CMPI(b), CMPI(c), 0));
}

int crypto_bignum_inverse(const struct crypto_bignum *a, const struct crypto_bignum *b, struct crypto_bignum *c)
{
    mbedtls_mpi r;
    mbedtls_mpi_init(&r);
    return into(c, &r, mbedtls_mpi_inv_mod(&r, CMPI(a), CMPI(b)));
}

int crypto_bignum_addmod(const struct crypto_bignum *a, const struct crypto_bignum *b,
                         const struct crypto_bignum *c, struct crypto_bignum *d)
{
    mbedtls_mpi r;
    mbedtls_mpi_init(&r);
    return mod_into(d, &r, c, mbedtls_mpi_add_mpi(&r, CMPI(a), CMPI(b)));
}

int crypto_bignum_mulmod(const struct crypto_bignum *a, const struct crypto_bignum *b,
                         const struct crypto_bignum *c, struct crypto_bignum *d)
{
    mbedtls_mpi r;
    mbedtls_mpi_init(&r);
    return mod_into(d, &r, c, mbedtls_mpi_mul_mpi(&r, CMPI(a), CMPI(b)));
}

int crypto_bignum_sqrmod(const struct crypto_bignum *a, const struct crypto_bignum *b, struct crypto_bignum *c)
{
    return crypto_bignum_mulmod(a, a, b, c);
}

int crypto_bignum_rshift(const struct crypto_bignum *a, int n, struct crypto_bignum *r)
{
    mbedtls_mpi t;
    mbedtls_mpi_init(&t);
    int status = n < 0 ? MBEDTLS_ERR_MPI_BAD_INPUT_DATA : mbedtls_mpi_copy(&t, CMPI(a));
    if (status == 0) {
        status = mbedtls_mpi_shift_r(&t, (size_t)n);
    }
    return into(r, &t, status);
}

int crypto_bignum_cmp(const struct crypto_bignum *a, const struct crypto_bignum *b)
{
    return mbedtls_mpi_cmp_mpi(CMPI(a), CMPI(b));
}

int crypto_bignum_is_zero(const struct crypto_bignum *a)
{
    return mbedtls_mpi_cmp_int(CMPI(a), 0) == 0;
}

int crypto_bignum_is_one(const struct crypto_bignum *a)
{
    return mbedtls_mpi_cmp_int(CMPI(a), 1) == 0;
}

int crypto_bignum_is_odd(const struct crypto_bignum *a)
{
    return mbedtls_mpi_get_bit(CMPI(a), 0);
}

/*
 * The Legendre symbol (a/p) of an odd prime p, by Euler's criterion: a^((p-1)/2) is 1, 0 or p-1 mod p.
 * hostap asks it of random numbers, or of a secret blinded at random, so the answer may branch.
 */
int crypto_bignum_legendre(const struct crypto_bignum *a, const struct crypto_bignum *p)
{
    mbedtls_mpi e, t;
    mbedtls_mpi_init(&e);
    mbedtls_mpi_init(&t);
    int res = -2;
    if (mbedtls_mpi_sub_int(&e, CMPI(p), 1) == 0 && mbedtls_mpi_shift_r(&e, 1) == 0 &&
        mbedtls_mpi_exp_mod(&t, CMPI(a), &e, CMPI(p), 0) == 0) {
        res = mbedtls_mpi_cmp_int(&t, 1) == 0 ? 1 : mbedtls_mpi_cmp_int(&t, 0) == 0 ? 0 : -1;
    }
    mbedtls_mpi_free(&e);
    mbedtls_mpi_free(&t);
    return res;
}

/* --- The curve. --- */

struct crypto_ec *crypto_ec_init(int group)
{
    if (group != GROUP_P256) {
        return 0;
    }
    struct crypto_ec *e = os_zalloc(sizeof(*e));
    if (e == 0) {
        return 0;
    }
    mbedtls_ecp_group_init(&e->group);
    mbedtls_mpi_init(&e->a);
    if (mbedtls_ecp_group_load(&e->group, MBEDTLS_ECP_DP_SECP256R1) != 0 ||
        mbedtls_mpi_sub_int(&e->a, &e->group.P, 3) != 0) {
        crypto_ec_deinit(e);
        return 0;
    }
    return e;
}

void crypto_ec_deinit(struct crypto_ec *e)
{
    if (e) {
        mbedtls_ecp_group_free(&e->group);
        mbedtls_mpi_free(&e->a);
        os_free(e);
    }
}

size_t crypto_ec_prime_len(struct crypto_ec *e)
{
    return mbedtls_mpi_size(&e->group.P);
}

size_t crypto_ec_prime_len_bits(struct crypto_ec *e)
{
    return e->group.pbits;
}

size_t crypto_ec_order_len(struct crypto_ec *e)
{
    return mbedtls_mpi_size(&e->group.N);
}

const struct crypto_bignum *crypto_ec_get_prime(struct crypto_ec *e)
{
    return (const struct crypto_bignum *)&e->group.P;
}

const struct crypto_bignum *crypto_ec_get_order(struct crypto_ec *e)
{
    return (const struct crypto_bignum *)&e->group.N;
}

const struct crypto_bignum *crypto_ec_get_a(struct crypto_ec *e)
{
    return (const struct crypto_bignum *)&e->a;
}

const struct crypto_bignum *crypto_ec_get_b(struct crypto_ec *e)
{
    return (const struct crypto_bignum *)&e->group.B;
}

/* SAE over a finite field's group, of dh_groups.c, which the driver does not build. */
const struct dh_group *dh_groups_get(int id)
{
    (void)id;
    return 0;
}

/* --- Points. --- */

/* A point Mbed TLS initializes is all zero, the point at infinity. */
struct crypto_ec_point *crypto_ec_point_init(struct crypto_ec *e)
{
    (void)e;
    mbedtls_ecp_point *p = os_malloc(sizeof(*p));
    if (p) {
        mbedtls_ecp_point_init(p);
    }
    return (struct crypto_ec_point *)p;
}

void crypto_ec_point_deinit(struct crypto_ec_point *p, int clear)
{
    (void)clear;
    if (p) {
        mbedtls_ecp_point_free(POINT(p));
        os_free(p);
    }
}

static int at_infinity(const struct crypto_ec_point *p)
{
    return mbedtls_mpi_cmp_int(&Z(CPOINT(p)), 0) == 0;
}

int crypto_ec_point_is_at_infinity(struct crypto_ec *e, const struct crypto_ec_point *p)
{
    (void)e;
    return at_infinity(p);
}

int crypto_ec_point_to_bin(struct crypto_ec *e, const struct crypto_ec_point *point, u8 *x, u8 *y)
{
    size_t len = crypto_ec_prime_len(e);
    if (at_infinity(point)) {
        return -1;
    }
    if (x && mbedtls_mpi_write_binary(&X(CPOINT(point)), x, len) != 0) {
        return -1;
    }
    if (y && mbedtls_mpi_write_binary(&Y(CPOINT(point)), y, len) != 0) {
        return -1;
    }
    return 0;
}

/* A point on the curve, or none, as OpenSSL's backend checks too. */
struct crypto_ec_point *crypto_ec_point_from_bin(struct crypto_ec *e, const u8 *val)
{
    size_t len = crypto_ec_prime_len(e);
    struct crypto_ec_point *p = crypto_ec_point_init(e);
    if (p == 0) {
        return 0;
    }
    if (mbedtls_mpi_read_binary(&X(POINT(p)), val, len) != 0 ||
        mbedtls_mpi_read_binary(&Y(POINT(p)), val + len, len) != 0 || mbedtls_mpi_lset(&Z(POINT(p)), 1) != 0 ||
        mbedtls_ecp_check_pubkey(&e->group, CPOINT(p)) != 0) {
        crypto_ec_point_deinit(p, 1);
        return 0;
    }
    return p;
}

int crypto_ec_point_add(struct crypto_ec *e, const struct crypto_ec_point *a, const struct crypto_ec_point *b,
                        struct crypto_ec_point *c)
{
    if (at_infinity(a) || at_infinity(b)) {
        return mbedtls_ecp_copy(POINT(c), CPOINT(at_infinity(a) ? b : a)) == 0 ? 0 : -1;
    }
    mbedtls_mpi one;
    mbedtls_mpi_init(&one);
    int r = mbedtls_mpi_lset(&one, 1);
    if (r == 0) {
        r = mbedtls_ecp_muladd(&e->group, POINT(c), &one, CPOINT(a), &one, CPOINT(b));
    }
    mbedtls_mpi_free(&one);
    return r == 0 ? 0 : -1;
}

int crypto_ec_point_mul(struct crypto_ec *e, const struct crypto_ec_point *p, const struct crypto_bignum *b,
                        struct crypto_ec_point *res)
{
    mbedtls_mpi m;
    mbedtls_mpi_init(&m);
    int r = mbedtls_mpi_mod_mpi(&m, CMPI(b), &e->group.N);
    if (r == 0 && (at_infinity(p) || mbedtls_mpi_cmp_int(&m, 0) == 0)) {
        r = mbedtls_ecp_set_zero(POINT(res));
    } else if (r == 0) {
        r = mbedtls_ecp_mul(&e->group, POINT(res), &m, CPOINT(p), rng, 0);
    }
    mbedtls_mpi_free(&m);
    return r == 0 ? 0 : -1;
}

int crypto_ec_point_invert(struct crypto_ec *e, struct crypto_ec_point *p)
{
    if (at_infinity(p) || mbedtls_mpi_cmp_int(&Y(POINT(p)), 0) == 0) {
        return 0;
    }
    return crypto_bignum_sub(crypto_ec_get_prime(e), (const struct crypto_bignum *)&Y(POINT(p)),
                             (struct crypto_bignum *)&Y(POINT(p)));
}

/* y^2 = x^3 + ax + b mod p, as (x^2 + a)x + b. */
struct crypto_bignum *crypto_ec_point_compute_y_sqr(struct crypto_ec *e, const struct crypto_bignum *x)
{
    const struct crypto_bignum *p = crypto_ec_get_prime(e);
    struct crypto_bignum *y2 = crypto_bignum_init();
    if (y2 == 0 || crypto_bignum_sqrmod(x, p, y2) < 0 || crypto_bignum_addmod(y2, crypto_ec_get_a(e), p, y2) < 0 ||
        crypto_bignum_mulmod(y2, x, p, y2) < 0 || crypto_bignum_addmod(y2, crypto_ec_get_b(e), p, y2) < 0) {
        crypto_bignum_deinit(y2, 1);
        return 0;
    }
    return y2;
}

/* Whether p lies on the curve, which the point at infinity does not here. */
int crypto_ec_point_is_on_curve(struct crypto_ec *e, const struct crypto_ec_point *p)
{
    return mbedtls_ecp_check_pubkey(&e->group, CPOINT(p)) == 0;
}

int crypto_ec_point_cmp(const struct crypto_ec *e, const struct crypto_ec_point *a, const struct crypto_ec_point *b)
{
    (void)e;
    return mbedtls_ecp_point_cmp(CPOINT(a), CPOINT(b)) == 0 ? 0 : 1;
}
