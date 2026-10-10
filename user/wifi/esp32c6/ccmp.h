#ifndef RVUOS_WIFI_CCMP_H
#define RVUOS_WIFI_CCMP_H

/*
 * IEEE 802.11's CCMP in software, on hostap's CCM, crypto/aes-ccm.c: the sending of the driver's own station,
 * and both directions of its host test. The receiving is the MAC's cipher, see mac.c; the decryption here is
 * what the test holds the encryption against. See TODO.md.
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
 * The 802.11 header's length of the frame at h: the 24 bytes of a Data frame or the 26 of a QoS Data one,
 * or 0 for any other frame, which a protected management frame is and which has no CCMP header here.
 */
uint32_t ccmp_header_len(const uint8_t *h);

/*
 * The priority of the frame at h: the traffic identifier a QoS Data frame's Control field names, or 0.
 * It is the byte of the nonce and the additional data that CCMP authenticates, and so the counter
 * a received frame's packet number is checked against; a frame without a QoS Control field has priority 0.
 */
uint32_t ccmp_priority(const uint8_t *h);

/*
 * The additional authentication data of the frame at h into a, 22 bytes, or 24 for a QoS Data frame; and its
 * nonce into n, 13 bytes. The test holds them against the standard's vectors, the management one among them.
 */
uint32_t ccmp_aad(uint8_t *a, const uint8_t *h);
void ccmp_nonce(uint8_t *n, const uint8_t *h, uint64_t pn);

/*
 * The replay counters a key keeps: one a priority, and one more, CCMP_REPLAY_MGMT, reserved for a robust
 * management frame, which read_frame drops today and a protected one under PMF will want.
 */
#define CCMP_REPLAY_COUNT 17u
#define CCMP_REPLAY_MGMT  16u /* the counter a robust management frame's packet number is checked against */

/* The replay check's outcomes, from ccmp_replay. */
#define CCMP_REPLAY_TAKEN 1    /* above the counter, which is then set to it */
#define CCMP_REPLAY_COPY  0    /* equal to it: the same frame again */
#define CCMP_REPLAY_OLD   (-1) /* below it: a replay */

/*
 * pn against the counters at idx, which hold CCMP_REPLAY_COUNT of them: CCMP_REPLAY_TAKEN, the counter set to pn,
 * CCMP_REPLAY_COPY if pn equals it, or CCMP_REPLAY_OLD if it is below; an index past the counters reads as old.
 */
int ccmp_replay(uint64_t pn, uint32_t idx, uint64_t *counters);

/* Every counter set to rsc, the receive sequence counter a handshake gives a key, so its frames start above it. */
void ccmp_replay_start(uint64_t *counters, uint64_t rsc);

/*
 * What a received frame is, from the Protected bit of its Frame Control and whether the MAC's cipher took it.
 * A frame without the Protected bit is in the clear whatever the flag says, since the cipher leaves the bit set
 * on a frame it took (mac.h); a protected one the cipher took is CCMP's, and one it did not is to be dropped.
 */
#define CCMP_FRAME_CLEAR 0
#define CCMP_FRAME_CCMP  1
#define CCMP_FRAME_DROP  2
int ccmp_frame_kind(const uint8_t *h, int decrypted);

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
 * The len bytes of the frame at in laid out for the MAC's cipher to encrypt: the header with its Protected bit set,
 * the CCMP header ccmp_encrypt builds, the payload left in the clear and the MIC's room set to zero, len + 16 bytes,
 * returned. The MAC writes the ciphertext and the MIC, given the key entry its PLCP1 keyslot field names.
 * 0 if it does not fit, or is not a Data frame of a header this reads.
 * ccmp_encrypt and this agree on the header, the CCMP header and the frame's length, and differ in the payload
 * (the ciphertext against the clear) and the MIC, which ccmp-test's hw_path holds.
 */
uint32_t ccmp_encap_hw(uint8_t *out, uint32_t size, const uint8_t *in, uint32_t len, uint64_t pn, uint8_t keyid);

/*
 * The len bytes of a robust management frame at in encrypted into out, as ccmp_encrypt for a Data frame:
 * its header is 24 bytes and no traffic identifier enters the nonce or the additional data.
 * 0 if it does not fit, or is not a management frame; else the frame's length at out, len + 16 bytes.
 */
uint32_t ccmp_encrypt_mgmt(uint8_t *out, uint32_t size, const uint8_t *in, uint32_t len, const uint8_t *tk,
                           uint64_t pn, uint8_t keyid);

/*
 * The packet number and key id the CCMP header at c carries, the inverse of the header ccmp_encrypt writes;
 * 0 if the Extended IV bit is clear, so a frame without a CCMP header reads as none.
 * A receiver that takes a frame the MAC's cipher already decrypted reads its header back with this.
 */
int ccmp_head_read(const uint8_t *c, uint64_t *pn, uint8_t *keyid);

/*
 * The len bytes of the protected frame at in decrypted into out, which holds at least len bytes:
 * the header with its Protected bit cleared and its payload, len - 16 bytes, whose length is put into *out_len.
 * Its packet number and key id are put into *pn and *keyid.
 * 1 if the frame is a Data frame of a header this reads and its MIC held, 0 otherwise, when nothing is decrypted.
 */
int ccmp_decrypt(uint8_t *out, uint32_t size, const uint8_t *in, uint32_t len, const uint8_t *tk, uint64_t *pn,
                 uint8_t *keyid, uint32_t *out_len);

/* As ccmp_decrypt for a robust management frame; its header is 24 bytes and no traffic identifier enters. */
int ccmp_decrypt_mgmt(uint8_t *out, uint32_t size, const uint8_t *in, uint32_t len, const uint8_t *tk, uint64_t *pn,
                      uint8_t *keyid, uint32_t *out_len);

#endif
