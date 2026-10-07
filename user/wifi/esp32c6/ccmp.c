/* IEEE 802.11's CCMP in software; see ccmp.h. */

#include "utils/includes.h"

#include "utils/common.h"

#include "common/ieee802_11_defs.h"

#include "crypto/aes_wrap.h"

#include "ccmp.h"

/*
 * The Frame Control and Sequence Control fields as the additional authentication data carries them:
 * the subtype's low three bits, the Retry, Power Management and More Data bits, and the sequence number,
 * are masked to zero, and for a QoS Data frame the Order bit too;
 * the Protected bit is always set, whether the frame being read says so or not.
 */
#define AAD_FC_MASK  0xC78Fu
#define AAD_ORDER    0x8000u
#define AAD_SEQ_MASK 0x000Fu

#define AAD_MAX        24u /* Frame Control, three addresses, Sequence Control and a QoS Control field */
#define CCMP_NONCE_LEN 13u /* the priority, Address 2 and the packet number */
#define EXT_IV         0x20u /* the Extended IV bit of the CCMP header's Key ID octet, always set */

/* A Data frame's header length: 24 bytes, 26 with the QoS Control field, or 0 for any other frame. */
uint32_t ccmp_header_len(const uint8_t *h)
{
    uint16_t fc = (uint16_t)h[0] | (uint16_t)h[1] << 8;
    if (WLAN_FC_GET_TYPE(fc) != WLAN_FC_TYPE_DATA) {
        return 0;
    }
    switch (WLAN_FC_GET_STYPE(fc)) {
    case WLAN_FC_STYPE_DATA:
        return 24u;
    case WLAN_FC_STYPE_QOS_DATA:
        return 26u;
    default:
        return 0;
    }
}

/* The frame's QoS Data subtype: its header is two bytes longer and its TID enters the nonce and the AAD. */
static int is_qos(const uint8_t *h)
{
    return ccmp_header_len(h) == 26u;
}

/*
 * The frame's priority: the traffic identifier its QoS Control field names, or 0 without one.
 * The nonce, the additional data and the replay counter's index all read it here, and nowhere else.
 */
uint32_t ccmp_priority(const uint8_t *h)
{
    return is_qos(h) ? (h[24] & 0x0fu) : 0u;
}

/*
 * The additional authentication data of the frame at h: Frame Control, Address 1 to 3 and Sequence Control,
 * then the QoS Control field's TID alone;
 * the Duration field is not part of it, which is why it is built here rather than masked in place.
 * Returns the length, 22 or 24.
 */
static uint32_t aad(uint8_t *a, const uint8_t *h, int qos)
{
    uint16_t fc = ((uint16_t)h[0] | (uint16_t)h[1] << 8) & AAD_FC_MASK;
    uint16_t sc = ((uint16_t)h[22] | (uint16_t)h[23] << 8) & AAD_SEQ_MASK;
    if (qos) {
        fc &= (uint16_t)~AAD_ORDER;
    }
    fc |= WLAN_FC_PROTECTED;
    a[0] = (uint8_t)fc;
    a[1] = (uint8_t)(fc >> 8);
    memcpy(a + 2, h + 4, 6);   /* Address 1 */
    memcpy(a + 8, h + 10, 6);  /* Address 2 */
    memcpy(a + 14, h + 16, 6); /* Address 3 */
    a[20] = (uint8_t)sc;
    a[21] = (uint8_t)(sc >> 8);
    if (qos) {
        a[22] = (uint8_t)ccmp_priority(h); /* the priority alone; the rest of the QoS Control field is reserved */
        a[23] = 0;
        return 24u;
    }
    return 22u;
}

/* The nonce of the frame at h: the priority, Address 2, and the packet number most significant octet first. */
static void nonce(uint8_t *n, const uint8_t *h, uint64_t pn)
{
    n[0] = (uint8_t)ccmp_priority(h);
    memcpy(n + 1, h + 10, 6); /* Address 2 */
    for (uint32_t i = 0; i < 6; i++) {
        n[7 + i] = (uint8_t)(pn >> (8u * (5u - i)));
    }
}

/* The CCMP header the frame gains, the packet number and the key id, the Extended IV bit always set. */
static void head(uint8_t *c, uint64_t pn, uint8_t keyid)
{
    c[0] = (uint8_t)pn;
    c[1] = (uint8_t)(pn >> 8);
    c[2] = 0;
    c[3] = EXT_IV | (uint8_t)((keyid & 3u) << 6);
    c[4] = (uint8_t)(pn >> 16);
    c[5] = (uint8_t)(pn >> 24);
    c[6] = (uint8_t)(pn >> 32);
    c[7] = (uint8_t)(pn >> 40);
}

/*
 * The packet number and key id the CCMP header at c carries, head's inverse;
 * 0 if the Extended IV bit is clear, so a frame without a CCMP header reads as none.
 * The packet number's six bytes are not contiguous, which is the reading that is easy to get wrong.
 */
int ccmp_head_read(const uint8_t *c, uint64_t *pn, uint8_t *keyid)
{
    if ((c[3] & EXT_IV) == 0) {
        return 0;
    }
    *pn = (uint64_t)c[0] | (uint64_t)c[1] << 8 | (uint64_t)c[4] << 16 | (uint64_t)c[5] << 24 |
          (uint64_t)c[6] << 32 | (uint64_t)c[7] << 40;
    *keyid = (uint8_t)((c[3] >> 6) & 3u);
    return 1;
}

/* The replay check, one counter a priority; see ccmp.h. */
int ccmp_replay(uint64_t pn, uint32_t idx, uint64_t *counters)
{
    if (idx >= CCMP_REPLAY_COUNT) {
        return CCMP_REPLAY_OLD;
    }
    if (pn < counters[idx]) {
        return CCMP_REPLAY_OLD;
    }
    if (pn == counters[idx]) {
        return CCMP_REPLAY_COPY;
    }
    counters[idx] = pn;
    return CCMP_REPLAY_TAKEN;
}

/* Every counter set to rsc; see ccmp.h. */
void ccmp_replay_start(uint64_t *counters, uint64_t rsc)
{
    for (uint32_t i = 0; i < CCMP_REPLAY_COUNT; i++) {
        counters[i] = rsc;
    }
}

/*
 * What a received frame is, from its Frame Control and the MAC's flag; see ccmp.h.
 * The Protected bit is the frame's own, so a frame in the clear is read as one whatever the flag holds.
 */
int ccmp_frame_kind(const uint8_t *h, int decrypted)
{
    if ((h[1] & (uint8_t)(WLAN_FC_PROTECTED >> 8)) == 0) {
        return CCMP_FRAME_CLEAR;
    }
    return decrypted ? CCMP_FRAME_CCMP : CCMP_FRAME_DROP;
}

uint32_t ccmp_encrypt(uint8_t *out, uint32_t size, const uint8_t *in, uint32_t len, const uint8_t *tk, uint64_t pn,
                      uint8_t keyid)
{
    uint32_t hdrlen = ccmp_header_len(in);
    if (hdrlen == 0 || hdrlen > len || len > size || size - len < CCMP_HEAD_LEN + CCMP_MIC_LEN) {
        return 0;
    }
    int qos = is_qos(in);
    uint32_t plen = len - hdrlen;
    uint8_t a[AAD_MAX], v[CCMP_NONCE_LEN], mic[CCMP_MIC_LEN];

    memcpy(out, in, hdrlen);
    out[1] |= (uint8_t)(WLAN_FC_PROTECTED >> 8); /* the Protected bit, whose second octet holds it */
    uint32_t alen = aad(a, out, qos);
    nonce(v, out, pn);

    /*
     * hostap's CCM encrypts only into a buffer of its own, and writes a whole block for a last one part filled,
     * so the ciphertext is written where the payload lies and the CCMP header is made room for after:
     * that last block reaches no further than the frame's own CCMP header and MIC will, len + 16 bytes.
     */
    if (aes_ccm_ae(tk, CCMP_TK_LEN, v, CCMP_MIC_LEN, in + hdrlen, plen, a, alen, out + hdrlen, mic) != 0) {
        return 0;
    }
    memmove(out + hdrlen + CCMP_HEAD_LEN, out + hdrlen, plen);
    head(out + hdrlen, pn, keyid);
    memcpy(out + hdrlen + CCMP_HEAD_LEN + plen, mic, CCMP_MIC_LEN);
    return len + CCMP_HEAD_LEN + CCMP_MIC_LEN;
}

int ccmp_decrypt(uint8_t *out, uint32_t size, const uint8_t *in, uint32_t len, const uint8_t *tk, uint64_t *pn,
                 uint8_t *keyid, uint32_t *out_len)
{
    uint32_t hdrlen = ccmp_header_len(in);
    if (hdrlen == 0 || hdrlen + CCMP_HEAD_LEN + CCMP_MIC_LEN > len || len > size) {
        return 0;
    }
    int qos = is_qos(in);
    const uint8_t *c = in + hdrlen;
    uint32_t plen = len - hdrlen - CCMP_HEAD_LEN - CCMP_MIC_LEN;
    uint64_t p;
    uint8_t id;
    if (!ccmp_head_read(c, &p, &id)) {
        return 0;
    }
    uint8_t a[AAD_MAX], v[CCMP_NONCE_LEN];

    memcpy(out, in, hdrlen);
    out[1] &= (uint8_t)~(WLAN_FC_PROTECTED >> 8);
    uint32_t alen = aad(a, out, qos);
    nonce(v, out, p);
    if (aes_ccm_ad(tk, CCMP_TK_LEN, v, CCMP_MIC_LEN, c + CCMP_HEAD_LEN, plen, a, alen, in + len - CCMP_MIC_LEN,
                   out + hdrlen) != 0) {
        return 0;
    }
    *pn = p;
    *keyid = id;
    *out_len = hdrlen + plen;
    return 1;
}
