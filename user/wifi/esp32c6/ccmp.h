#ifndef RVUOS_WIFI_CCMP_H
#define RVUOS_WIFI_CCMP_H

/*
 * IEEE 802.11's CCMP in software, on hostap's CCM, crypto/aes-ccm.c: the data path of the driver's own station
 * while the MAC's key registers are not programmed yet; see TODO.md.
 * A frame is the 802.11 frame without its FCS, as mac.c and mgmt.c carry it, and its header is
 * the 24 bytes of a Data frame or the 26 of a QoS Data one, without an Address 4 or an HT control field.
 * The CCMP header the two add is the 8 bytes after the frame's own header, and the MIC the 8 bytes at its end,
 * so a protected frame is 16 bytes longer than the frame the caller built.
 * The packet number is the frame's, 48 bits, which the caller keeps and never repeats under one key;
 * the key id is the one the supplicant installed the key under, 0 for the pairwise key and 1 to 3 for a group key.
 * A receiver reads the packet number and the key id back out of the frame.
 * The two make no system call and run on the host too, under the sanitizers, test/ccmp-test.c.
 */

#include <stdint.h>

#define CCMP_TK_LEN   16u /* the temporal key's bytes, AES-128's */
#define CCMP_HEAD_LEN 8u  /* the CCMP header the frame gains */
#define CCMP_MIC_LEN  8u  /* the MIC the frame gains */

/*
 * The len bytes of the frame at in encrypted into out, which holds at least len + 16 bytes:
 * the header with its Protected bit set, the CCMP header, the ciphertext and the MIC, len + 16 bytes, returned.
 * The frame's payload begins after its header, whose length the frame control gives.
 * 0 if it does not fit, or is not a Data frame of a header this reads.
 * The packet number pn is written into the frame and the nonce, and the key id into the CCMP header.
 */
uint32_t ccmp_encrypt(uint8_t *out, uint32_t size, const uint8_t *in, uint32_t len, const uint8_t *tk, uint64_t pn,
                      uint8_t keyid);

/*
 * The len bytes of the protected frame at in decrypted into out, which holds at least len bytes:
 * the header with its Protected bit cleared and its payload, len - 16 bytes, whose length is put into *out_len.
 * Its packet number and key id are put into *pn and *keyid.
 * 1 if the frame is a Data frame of a header this reads and its MIC held, 0 otherwise, when nothing is decrypted.
 */
int ccmp_decrypt(uint8_t *out, uint32_t size, const uint8_t *in, uint32_t len, const uint8_t *tk, uint64_t *pn,
                 uint8_t *keyid, uint32_t *out_len);

#endif
