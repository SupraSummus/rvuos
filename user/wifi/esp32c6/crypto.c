/*
 * The crypto functions Espressif's libraries call through the configuration of esp_wifi_init_internal,
 * struct crypto_funcs of esp.h, on hostap's own crypto, which the supplicant runs on too.
 * The MAC encrypts and decrypts the data in CCMP itself, so the libraries' own CCMP and GMAC are left out:
 * joined to a network that protects management frames, the libraries did not call the CCMP they were given.
 */

#include "utils/common.h"

#include "crypto/aes_wrap.h"
#include "crypto/crypto.h"
#include "crypto/sha1.h"
#include "crypto/sha256.h"

#include "esp.h"
#include "osi.h"

/* The libraries' vectors have lengths as int, hostap's as size_t; as many elements as a KDF of 802.11 hashes. */
#define ELEMS 8

static int table_hmac_sha256(const unsigned char *key, int key_len, int num_elem, const unsigned char *addr[],
                             const int *len, unsigned char *mac)
{
    size_t lens[ELEMS];
    if (key_len < 0 || num_elem < 0 || num_elem > ELEMS) {
        return -1;
    }
    for (int i = 0; i < num_elem; i++) {
        if (len[i] < 0) {
            return -1;
        }
        lens[i] = (size_t)len[i];
    }
    return hmac_sha256_vector(key, (size_t)key_len, (size_t)num_elem, addr, lens, mac);
}

static int pbkdf2(const char *passphrase, const char *ssid, size_t ssid_len, int iterations, unsigned char *buf,
                  size_t buflen)
{
    return pbkdf2_sha1(passphrase, (const u8 *)ssid, ssid_len, iterations, buf, buflen);
}

static int cbc_encrypt(const unsigned char *key, const unsigned char *iv, unsigned char *data, int data_len)
{
    return data_len < 0 ? -1 : aes_128_cbc_encrypt(key, iv, data, (size_t)data_len);
}

static int cbc_decrypt(const unsigned char *key, const unsigned char *iv, unsigned char *data, int data_len)
{
    return data_len < 0 ? -1 : aes_128_cbc_decrypt(key, iv, data, (size_t)data_len);
}

static int omac1(const uint8_t *key, const uint8_t *data, size_t data_len, uint8_t *mic)
{
    return omac1_aes_128(key, data, data_len, mic);
}

static int wrap(const unsigned char *kek, size_t kek_len, int n, const unsigned char *plain, unsigned char *cipher)
{
    return aes_wrap(kek, kek_len, n, plain, cipher);
}

static int unwrap(const unsigned char *kek, size_t kek_len, int n, const unsigned char *cipher, unsigned char *plain)
{
    return aes_unwrap(kek, kek_len, n, cipher, plain);
}

void drv_crypto_funcs(struct crypto_funcs *c)
{
    c->size = sizeof(*c);
    c->version = CRYPTO_VERSION;
    c->hmac_sha256_vector = table_hmac_sha256;
    c->pbkdf2_sha1 = pbkdf2;
    c->aes_128_encrypt = cbc_encrypt;
    c->aes_128_decrypt = cbc_decrypt;
    c->omac1_aes_128 = omac1;
    c->sha256_vector = sha256_vector;
    c->aes_wrap = wrap;
    c->aes_unwrap = unwrap;
}
