/*
 * ccmp.c on the host, under the sanitizers.
 * The frame is held against IEEE 802.11's own CCMP test vector, whose header has Retry set and a sequence number,
 * so it covers the mask of both, and a More Fragments case beside it, which an over-broad Frame Control mask fails;
 * a QoS Data frame against an answer computed by 802.11's rules; then a changed key or MIC refused,
 * and a management frame refused.
 * Each frame lies in a buffer of its own length, so that a byte read or written past it fails the run.
 */

#include "utils/includes.h"

#include "utils/common.h"

#include "ccmp.h"

static int failures;

static void check(int ok, const char *what)
{
    if (!ok) {
        printf("ccmp-test: FAIL %s\n", what);
        failures++;
    }
}

static int same(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    return memcmp(a, b, n) == 0;
}

/* IEEE 802.11-2020 J.6.4: TK, a Data frame's header, its payload, and the frame encrypted. */
static const uint8_t tk[16] = { 0xc9, 0x7c, 0x1f, 0x67, 0xce, 0x37, 0x11, 0x85,
                                0x51, 0x4a, 0x8a, 0x19, 0xf2, 0xbd, 0xd5, 0x2f };
#define PN 0xB5039776E70Cull
static const uint8_t header[24] = { 0x08, 0x08, 0xc3, 0x2c, 0x0f, 0xd2, 0xe1, 0x28, 0xa5, 0x7c, 0x50, 0x30,
                                    0xf1, 0x84, 0x44, 0x08, 0xab, 0xae, 0xa5, 0xb8, 0xfc, 0xba, 0x80, 0x33 };
static const uint8_t plain[20] = { 0xf8, 0xba, 0x1a, 0x55, 0xd0, 0x2f, 0x85, 0xae, 0x96, 0x7b,
                                   0xb6, 0x2f, 0xb6, 0xcd, 0xa8, 0xeb, 0x7e, 0x78, 0xa0, 0x50 };
static const uint8_t known[24 + 8 + 20 + 8] = {
    0x08, 0x48, 0xc3, 0x2c, 0x0f, 0xd2, 0xe1, 0x28, 0xa5, 0x7c, 0x50, 0x30, 0xf1, 0x84, 0x44, 0x08,
    0xab, 0xae, 0xa5, 0xb8, 0xfc, 0xba, 0x80, 0x33, 0x0c, 0xe7, 0x00, 0x20, 0x76, 0x97, 0x03, 0xb5,
    0xf3, 0xd0, 0xa2, 0xfe, 0x9a, 0x3d, 0xbf, 0x23, 0x42, 0xa6, 0x43, 0xe4, 0x32, 0x46, 0xe8, 0x0c,
    0x3c, 0x04, 0xd0, 0x19, 0x78, 0x45, 0xce, 0x0b, 0x16, 0xf9, 0x76, 0x23
};

/* The known frame, and with More Fragments set in its Frame Control, which the additional data carries. */
static void known_answer(void)
{
    const uint8_t mf_mic[8] = { 0x96, 0x0b, 0xb1, 0x37, 0x28, 0x48, 0x24, 0x1a };
    uint8_t out[sizeof(known) + 8], in[24 + sizeof(plain)], back[sizeof(known)];
    uint64_t pn = 0;
    uint8_t keyid = 0xff;
    uint32_t out_len = 0;

    memcpy(in, header, 24);
    memcpy(in + 24, plain, sizeof(plain));
    uint32_t n = ccmp_encrypt(out, sizeof(out), in, sizeof(in), tk, PN, 0);
    check(n == sizeof(known) && same(out, known, sizeof(known)), "IEEE 802.11's CCMP vector encrypted");
    check(ccmp_decrypt(back, sizeof(back), out, n, tk, &pn, &keyid, &out_len) && out_len == sizeof(in) &&
              same(back, in, sizeof(in)) && pn == PN && keyid == 0,
          "IEEE 802.11's CCMP vector decrypted");

    /* The header alone, read back: its packet number's six bytes all differ, so a swapped pair fails. */
    uint64_t hpn = 0;
    uint8_t hkeyid = 0xff;
    check(ccmp_head_read(known + 24, &hpn, &hkeyid) && hpn == PN && hkeyid == 0, "the CCMP header read back");

    in[1] |= 0x04; /* More Fragments */
    n = ccmp_encrypt(out, sizeof(out), in, sizeof(in), tk, PN, 0);
    check(n == sizeof(known) && same(out + 52, mf_mic, sizeof(mf_mic)), "More Fragments enters the MIC");
}

/*
 * A QoS Data frame's header is 26 bytes, and its TID enters the nonce and the additional data:
 * the known frame made QoS Data, TID 6, under key id 2 and packet number 9,
 * against an answer Python's cryptography computed by 802.11's rules, not by ccmp.c's.
 */
static void qos(void)
{
    static const uint8_t qos_known[26 + 8 + 16 + 8] = {
        0x88, 0x48, 0xc3, 0x2c, 0x0f, 0xd2, 0xe1, 0x28, 0xa5, 0x7c, 0x50, 0x30, 0xf1, 0x84, 0x44, 0x08,
        0xab, 0xae, 0xa5, 0xb8, 0xfc, 0xba, 0x80, 0x33, 0x06, 0x00, 0x09, 0x00, 0x00, 0xa0, 0x00, 0x00,
        0x00, 0x00, 0x51, 0xbf, 0x9f, 0xb2, 0xb3, 0xab, 0xb4, 0xe7, 0xe8, 0xc6, 0xef, 0x97, 0x19, 0x06,
        0xb5, 0x59, 0xa7, 0xab, 0x19, 0xa3, 0x9a, 0x76, 0xc8, 0x82
    };
    uint8_t in[26 + 16], out[sizeof(qos_known) + 8], back[sizeof(qos_known)];
    uint64_t pn = 0;
    uint8_t keyid = 0xff;
    uint32_t out_len = 0;

    memcpy(in, header, 24);
    in[0] = 0x88; /* the QoS Data subtype */
    in[24] = 0x06; /* TID 6 */
    in[25] = 0x00;
    memcpy(in + 26, plain, 16);
    uint32_t n = ccmp_encrypt(out, sizeof(out), in, sizeof(in), tk, 9, 2);
    check(n == sizeof(qos_known) && same(out, qos_known, sizeof(qos_known)), "a QoS frame encrypted to its known answer");
    check(ccmp_decrypt(back, sizeof(back), out, n, tk, &pn, &keyid, &out_len) && out_len == sizeof(in) &&
              same(back, in, sizeof(in)) && pn == 9 && keyid == 2,
          "a QoS frame decrypted");
}

static void refuses(void)
{
    uint8_t in[24 + sizeof(plain)], out[sizeof(known)], back[sizeof(known)];
    uint64_t pn = 0;
    uint8_t keyid = 0;
    uint32_t out_len = 0;

    memcpy(in, header, 24);
    memcpy(in + 24, plain, sizeof(plain));
    check(ccmp_encrypt(out, sizeof(out), in, sizeof(in), tk, 1, 0) == sizeof(known), "the frame encrypted");
    check(ccmp_encrypt(out, sizeof(known) - 1, in, sizeof(in), tk, 1, 0) == 0, "a frame too long for the buffer refused");
    in[0] = 0x80; /* a management frame */
    check(ccmp_encrypt(out, sizeof(out), in, sizeof(in), tk, 1, 0) == 0, "a management frame refused");
    in[0] = header[0];

    check(!ccmp_decrypt(back, sizeof(back), out, 20, tk, &pn, &keyid, &out_len), "a frame shorter than its header refused");
    check(!ccmp_decrypt(back, sizeof(back), out, sizeof(known) - 1, tk, &pn, &keyid, &out_len),
          "a frame without its whole MIC refused");
    out[sizeof(known) - 1] ^= 1;
    check(!ccmp_decrypt(back, sizeof(back), out, sizeof(known), tk, &pn, &keyid, &out_len), "a changed MIC refused");
    out[sizeof(known) - 1] ^= 1;
    uint8_t wrong[16];
    memcpy(wrong, tk, sizeof(wrong));
    wrong[0] ^= 1;
    check(!ccmp_decrypt(back, sizeof(back), out, sizeof(known), wrong, &pn, &keyid, &out_len), "another key refused");
    out[27] &= (uint8_t)~0x20;
    check(!ccmp_decrypt(back, sizeof(back), out, sizeof(known), tk, &pn, &keyid, &out_len),
          "a frame without the extended IV refused");
}

/*
 * The replay check, one counter a priority: a frame of one TID overtaken by a later-numbered one of another
 * is taken, the same number again is a copy, and one below its TID's counter is a replay;
 * a Data frame without QoS shares TID 0's counter, and a counter starts from the handshake's receive sequence counter.
 */
static void replay(void)
{
    uint64_t c[CCMP_REPLAY_COUNT] = { 0 };
    uint8_t qos6[26] = { 0x88 }, qos0[26] = { 0x88 }, data[24] = { 0x08 };
    qos6[24] = 6;
    uint32_t p6 = ccmp_priority(qos6), p0 = ccmp_priority(qos0), pd = ccmp_priority(data);
    check(p6 == 6 && p0 == 0 && pd == 0, "the priority is a QoS frame's TID, or 0 without one");

    check(ccmp_replay(10, p6, c) == CCMP_REPLAY_TAKEN, "the first frame of a TID taken");
    check(ccmp_replay(5, p0, c) == CCMP_REPLAY_TAKEN, "another TID's lower number taken");
    check(ccmp_replay(11, p6, c) == CCMP_REPLAY_TAKEN, "the first TID again, above, taken");
    check(ccmp_replay(11, p6, c) == CCMP_REPLAY_COPY, "the same number again a copy");
    check(ccmp_replay(9, p6, c) == CCMP_REPLAY_OLD, "below the TID's counter a replay");
    check(ccmp_replay(4, p0, c) == CCMP_REPLAY_OLD, "another TID's counter is its own");
    check(ccmp_replay(6, pd, c) == CCMP_REPLAY_TAKEN, "a Data frame's number extends TID 0's counter");
    check(ccmp_replay(6, p0, c) == CCMP_REPLAY_COPY, "and they share one counter");

    /* A key's counters start from the handshake's receive sequence counter, and an index past them reads as old. */
    uint64_t start[CCMP_REPLAY_COUNT];
    ccmp_replay_start(start, 100);
    check(ccmp_replay(100, p6, start) == CCMP_REPLAY_COPY, "a frame at the RSC is a copy");
    check(ccmp_replay(99, p6, start) == CCMP_REPLAY_OLD, "one below the RSC a replay");
    check(ccmp_replay(101, p6, start) == CCMP_REPLAY_TAKEN, "one above the RSC taken");
    check(ccmp_replay(1, CCMP_REPLAY_COUNT, start) == CCMP_REPLAY_OLD, "an index past the counters reads as old");
}

/*
 * What a frame is, from its Frame Control and the MAC's flag, in the two cases that are not the plain one:
 * a frame without the Protected bit, which is in the clear whatever the flag says,
 * and a protected frame the cipher did not take, which is dropped.
 */
static void frame_kind(void)
{
    uint8_t clear[24] = { 0x08 }; /* Data, the Protected bit clear */
    uint8_t prot[24] = { 0x08, 0x40 };
    check(ccmp_frame_kind(clear, 1) == CCMP_FRAME_CLEAR, "an unprotected frame the flag calls decrypted is still clear");
    check(ccmp_frame_kind(prot, 0) == CCMP_FRAME_DROP, "a protected frame the cipher did not take is dropped");
}

int main(void)
{
    known_answer();
    qos();
    refuses();
    replay();
    frame_kind();
    printf("ccmp-test: %s\n", failures ? "FAILED" : "ok");
    return failures != 0;
}
