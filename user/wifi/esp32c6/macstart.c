/*
 * The register sequences of the libraries' bring-up, written out, so that the driver's own start makes them:
 * their hal_init's groups, which mac_config below calls in its order, their hal_crypto_init's cipher words,
 * and their coex PTI. main.c's start calls mac_config where the libraries' hal_init called the groups;
 * the accesses each makes are the libraries', and the trace holds them.
 * They reach the device through macregs.h alone, so they build on the host for their replay;
 * the libraries' calls they leave are declared below.
 */

#include <stdint.h>

#include "mac.h"
#include "macregs.h"

/*
 * The libraries' hal_init's own register writes, around the groups of its MAC configuration that it calls:
 * their HAL_CFG start and the wait for it, and HAL_HOLD and INT_CLEAR cleared.
 * Then, after the groups, HAL_HOLD, HAL_WORD and RX_WORD as their hal_init leaves them.
 * The driver's own start makes these, and mac_config below runs them and the groups in the libraries' order.
 */
void mac_config_start(void)
{
    wr(HAL_CFG, rd(HAL_CFG) | 2u);
    while (!(rd(HAL_CFG) & 1u)) {
    }
    wr(HAL_HOLD, 0);
    wr(INT_CLEAR, 0xffffffffu);
}

/*
 * The libraries' mac_txrx_init, written out:
 * its sniffers cleared and set to their default, the words that set up its queues and hold the MAC,
 * the receive left disabled, which mac_rx_on sets again, and the three HE leaves at its middle, written out above.
 * The sniffers, HAL_WORD, HAL_CTRL and RX_CTRL are named in macregs.h, and DUMP_CTRL_FRAME and BBRXHUNG_TIME just
 * below; the others carry the offset they were reached at, since what they are is not known.
 * Each of their read-modify-writes is one here, in their order, since the trace holds each access.
 */
#define DUMP_CTRL_FRAME (MAC_BASE + 0x100u) /* what their hal_set_dump_ctrl_frame_cfg touches, beside the sniffers */
#define TXRX_MAC_110  (MAC_BASE + 0x110u)
#define TXRX_MAC_114  (MAC_BASE + 0x114u)
#define BBRXHUNG_TIME 0x600a4c1cu /* their hal_he_set_bbrxhung_time's low 12 bits; the txrx group's bits 30, 31 */
#define TXRX_HAL_C20  0x600a4c20u
#define TXRX_HAL_C24  0x600a4c24u
#define TXRX_HAL_C60  0x600a4c60u
#define TXRX_HAL_C98  0x600a4c98u
#define TXRX_HAL_C9C  0x600a4c9cu
#define TXRX_HAL_CA4  0x600a4ca4u
/*
 * The ack rate's and the cts rate's words: their dbg_read_ack_rate and dbg_read_cts_rate read them, and their
 * hal_he_set_ack_rate, hal_mac_enable_low_rate and hal_mac_disable_low_rate write them, at these constant addresses
 * alone; written out below.
 */
#define RATE_ACK0 (MAC_BASE + 0x440u)
#define RATE_ACK1 (MAC_BASE + 0x444u)
#define RATE_CTS0 (MAC_BASE + 0x44cu)
#define RATE_CTS1 (MAC_BASE + 0x450u)

/*
 * The libraries' hal_he_set_bbrxhung_time, written out, the HE word their txrx and low-rate groups both write:
 * BBRXHUNG_TIME's low 12 bits cleared, then 0x11 for the zero both calls pass and 0x46 otherwise.
 * Their name says a receive-hung timeout; what the field is is not known.
 */
static void mac_he_set_bbrxhung_time(uint32_t interval)
{
    uint32_t w = rd(BBRXHUNG_TIME) & 0xfffff000u;
    if (interval != 0) {
        w |= 0x46u;
    } else {
        w |= 0x11u;
    }
    wr(BBRXHUNG_TIME, w);
}

/*
 * The libraries' hal_he_set_mac_delay, written out: five read-modify-writes on the words 0x600a4c58 and 0x600a4c54.
 * Their body asks the adapter's env_is_chip first, which the board's answers true, so the chip's values are the ones
 * below and their other path is not written; their txrx group leaves a register's address in a0, which the body does
 * not read, so the leaf takes no argument.
 */
#define HE_MAC_DELAY0 0x600a4c54u
#define HE_MAC_DELAY1 0x600a4c58u

static void mac_he_set_mac_delay(void)
{
    wr(HE_MAC_DELAY1, (rd(HE_MAC_DELAY1) & 0xffe003ffu) | 0x123400u);
    wr(HE_MAC_DELAY1, (rd(HE_MAC_DELAY1) & ~0x3ffu) | 0xa0u);
    wr(HE_MAC_DELAY1, (rd(HE_MAC_DELAY1) & 0x801fffffu) | 0x0bc00000u);
    wr(HE_MAC_DELAY0, (rd(HE_MAC_DELAY0) & 0x801fffffu) | 0x14000000u);
    wr(HE_MAC_DELAY0, (rd(HE_MAC_DELAY0) & 0xffe003ffu) | 0x9d800u);
}

/*
 * The libraries' hal_he_set_ack_rate, written out: their four rate words given their values, 0x90a0b in 0x4440 and
 * 0x444c, 0xb0b0b there for a nonzero argument, and 0x50100 in 0x4444 and 0x4450; then their body reads 0x4440 seven
 * times and 0x444c seven times for its log, reads the own start makes too, with no log of them.
 */
static void mac_he_set_ack_rate(uint32_t rate)
{
    if (rate != 0) {
        wr(RATE_ACK0, 0xb0b0bu);
        wr(RATE_CTS0, 0xb0b0bu);
    } else {
        wr(RATE_ACK0, 0x90a0bu);
        wr(RATE_CTS0, 0x90a0bu);
    }
    wr(RATE_ACK1, 0x50100u);
    wr(RATE_CTS1, 0x50100u);
    for (uint32_t i = 0; i < 7u; i++) {
        (void)rd(RATE_ACK0);
    }
    for (uint32_t i = 0; i < 7u; i++) {
        (void)rd(RATE_CTS0);
    }
}

/* The ROM's low-rate toggle, which the driver's start still calls, as their rate group does. */
extern void phy_disable_low_rate(void);

void mac_queues_init(void)
{
    wr(HAL_WORD, rd(HAL_WORD) | 0x8080a000u);
    wr(HAL_WORD, rd(HAL_WORD) | 0x1000u);
    wr(HAL_WORD, rd(HAL_WORD) | 0x10000000u);
    wr(TXRX_HAL_C98, rd(TXRX_HAL_C98) & ~0x8u);

    wr(SNIFF_CTRL0, rd(SNIFF_CTRL0) & 0xffffu);
    wr(SNIFF_CTRL1, rd(SNIFF_CTRL1) & 0xffffu);
    wr(DUMP_CTRL_FRAME, rd(DUMP_CTRL_FRAME) & 0xffffu);
    wr(SNIFF_CTRL_TYPES, rd(SNIFF_CTRL_TYPES) & 0xffffu);
    wr(SNIFF_CTRL0, rd(SNIFF_CTRL0) | 0x01000000u);
    wr(SNIFF_CTRL1, rd(SNIFF_CTRL1) | 0x01000000u);
    wr(SNIFF_CTRL0, rd(SNIFF_CTRL0) | 0x04000000u);
    wr(SNIFF_CTRL1, rd(SNIFF_CTRL1) | 0x04000000u); /* the two bits together are SNIFF_CTRL_NONE */

    wr(HAL_WORD, rd(HAL_WORD) | 0x200u);
    wr(TXRX_MAC_110, rd(TXRX_MAC_110) | 1u);
    wr(TXRX_MAC_110, rd(TXRX_MAC_110) | 0x10u);
    wr(TXRX_MAC_114, rd(TXRX_MAC_114) | 0x80000000u);
    wr(TXRX_MAC_114, (rd(TXRX_MAC_114) & 0xf00fffffu) | 0x01b00000u);
    wr(TXRX_HAL_C9C, rd(TXRX_HAL_C9C) | 3u);

    /* Their first call leaves a register's address in a0, which its leaf does not read; the two after it pass zero. */
    mac_he_set_mac_delay();
    mac_he_set_ack_rate(0);
    mac_he_set_bbrxhung_time(0);

    wr(BBRXHUNG_TIME, rd(BBRXHUNG_TIME) | 0x80000000u);
    wr(BBRXHUNG_TIME, rd(BBRXHUNG_TIME) | 0x40000000u);
    wr(TXRX_HAL_C20, (rd(TXRX_HAL_C20) & 0xfffff000u) | 0xf0u);
    wr(TXRX_HAL_C24, (rd(TXRX_HAL_C24) & 0xfffff000u) | 0xf0u);
    wr(TXRX_HAL_CA4, (rd(TXRX_HAL_CA4) & ~0xf0u) | 0x40u);
    wr(TXRX_HAL_C60, rd(TXRX_HAL_C60) | 0x7fff0000u);
    wr(TXRX_HAL_C60, rd(TXRX_HAL_C60) | 0x80000000u);
    wr(HAL_CTRL, rd(HAL_CTRL) | 2u);
    wr(RX_CTRL, rd(RX_CTRL) & ~RX_CTRL_ENABLE);
}

/* The read-modify-writes their hal_init makes on an interface's receive-policy word, before each policy call. */
void mac_rx_policy_word(uint32_t iface)
{
    uint32_t a = RX_POLICY + 4u * iface;
    wr(a, rd(a) | 0x280u);
    wr(a, rd(a) & ~0x400u);
    wr(a, rd(a) | 0x5u);
    wr(a, rd(a) & ~0x2040u);
}

/*
 * The policy call their hal_init's loop makes for each interface, their hal_mac_rx_set_policy(i, 0, 0, 0),
 * after the word above: the policy's 0x410 and 0x40 cleared (together RX_POLICY_CLEAR),
 * the interface's bssid word's bits 30 and 31 cleared, but interface 1's bit 30 set, IF1_DEFAULT_BIT,
 * and its own address made invalid, STA_ADDR_FLAG, as its bssid is, BSSID_FLAG.
 * Their station's own call, hal_mac_rx_set_policy(0, 1, 1, 1), is the same with valid: its last two accesses set
 * the two flags rather than clear them, and mac_station_start makes it, for interface 0.
 * Their function takes three flags beyond the interface, zero in hal_init's loop and 1, 1, 1 in the station's call,
 * and touches nothing for interface 3.
 */
void mac_rx_set_policy(uint32_t iface, uint32_t valid)
{
    if (iface > 2u) {
        return; /* their function returns for interface 3, touching nothing */
    }
    uint32_t p = RX_POLICY + 4u * iface;
    uint32_t w = BSSID_HI + 8u * iface;    /* interface 1's is IF1_DEFAULT_WORD */
    uint32_t a = STA_ADDR_HI + 8u * iface; /* interface 1's is STA_ADDR2_HI */

    wr(p, rd(p) & ~0x410u);
    if (iface == 1u) {
        wr(w, rd(w) | IF1_DEFAULT_BIT);
    } else {
        wr(w, rd(w) & ~BSSID_HI_POLICY);
    }
    wr(p, rd(p) & ~0x40u);
    if (valid) {
        wr(w, rd(w) | BSSID_FLAG);
        wr(a, rd(a) | STA_ADDR_FLAG);
    } else {
        wr(w, rd(w) & ~BSSID_FLAG);
        wr(a, rd(a) & ~STA_ADDR_FLAG);
    }
}

/*
 * The libraries' mac_rxbuf_init, written out: the low byte of the word before RX_CTRL cleared, 0x600a407c,
 * whose other bits are not known, and the receive base pointed at the list the MAC fills first --
 * their own control block, wDevCtrl, in their start, and the driver's list, which mac.c makes, in its own.
 */
#define RXBUF_MAC_7C (MAC_BASE + 0x07cu)

void mac_rx_base_init(uint32_t base)
{
    wr(RXBUF_MAC_7C, rd(RXBUF_MAC_7C) & 0xffffff00u);
    wr(RX_BASE, base);
}

/*
 * The libraries' hal_he_init, written out: their hal_init's 802.11ax (HE) group, the words it writes itself and the
 * HE calls it makes, in its order. The calls' names give the mechanisms -- beamforming, trigger-based TX and its
 * power tables, ER SU, the transmitting minimum power, the broadcast RU, the UORA contention window, multi-BSSID and
 * the co-hosted BSS mask -- and each call keeps the argument their start passed.
 * Of the words it writes itself, the two that read as something are their table at 0x600a55f0, cleared, and the
 * transmitting minimum power's own word, 0x600a4400, whose bits 4 to 9 their minimum-power call sets to -11 and
 * where this group ors bit 17; three more name their library function, COLOR_BITMAP, COLOR_ISR and BEACON_CRC,
 * and the rest carry the offset they were reached at, since which bit is which is not known.
 */
#define HE_CTRL     0x600a4c80u /* cleared before the calls and set after them, whatever it gates */
#define HE_MAC_10C  (MAC_BASE + 0x10cu)
#define COLOR_BITMAP (MAC_BASE + 0x48u) /* bit 0 their hal_mac_color_clr_bitmap sets, dbg_read_color_collision reads */
#define COLOR_ISR 0x600a4c2cu /* bit 12, the interrupt their hal_mac_color_enable/disable_collision_isr gate */
#define HE_HAL_C88  0x600a4c88u
#define HE_HAL_CBC  0x600a4cbcu
#define BEACON_CRC (MAC_BASE + 0x2d4u) /* what their pwr_hal_set_beacon_filter_frame_crc_state sets, the ROM's */
#define HE_TX_MIN   (MAC_BASE + 0x400u)
#define HE_TABLE    0x600a55f0u /* their table, cleared */
#define HE_TABLE_END 0x600a57d0u

/*
 * The libraries' hal_he_set_ersu, written out: 0x600a4c7c's bit 10 cleared for a nonzero argument and set for the
 * zero their start passes, the word just before HE_CTRL. What the bit gates is not known.
 */
#define HE_ERSU 0x600a4c7cu

static void mac_he_set_ersu(uint32_t enable)
{
    if (enable != 0) {
        wr(HE_ERSU, rd(HE_ERSU) & ~0x400u);
    } else {
        wr(HE_ERSU, rd(HE_ERSU) | 0x400u);
    }
}

/*
 * The libraries' hal_he_set_co_hosted_bss, written out, the HE word their HE group and its multi-BSSID clear share,
 * MAC+0x20: bit 17 cleared and then bits 9 to 16 set, for the zero the driver's call passes as its first argument;
 * their other path sets bit 17 and writes a mask of the second argument's low five bits into bits 9 to 16, untaken.
 */
static void mac_he_set_co_hosted_bss(uint32_t enable, uint32_t mask)
{
    if (enable != 0) {
        wr(MAC_CONF(0), rd(MAC_CONF(0)) | 0x20000u);
        wr(MAC_CONF(0), (rd(MAC_CONF(0)) & 0xfffe01ffu) | (((0xffffffffu << (mask & 31u)) & 0xffu) << 9));
    } else {
        wr(MAC_CONF(0), rd(MAC_CONF(0)) & ~0x20000u);
        wr(MAC_CONF(0), rd(MAC_CONF(0)) | 0x1fe00u);
    }
}

/*
 * The libraries' hal_he_set_bcast_ru, written out: two read-modify-writes on MAC+0x38, bit 22 set and then the field
 * in bits 11 to 21 given the first argument, and four on MAC+0x3c, bit 11 set, the low 11 bits given the second
 * argument, bit 23 set and bits 12 to 22 given the third. Their name says a broadcast resource unit; the fields'
 * meaning is not known.
 */
static void mac_he_set_bcast_ru(uint32_t ru, uint32_t low, uint32_t high)
{
    wr(HE_AID, rd(HE_AID) | 0x400000u);
    wr(HE_AID, (rd(HE_AID) & 0xffc007ffu) | ((ru << 11) & 0x3ff800u));
    wr(HE_BCAST, rd(HE_BCAST) | 0x800u);
    wr(HE_BCAST, (rd(HE_BCAST) & ~0x7ffu) | (low & 0x7ffu));
    wr(HE_BCAST, rd(HE_BCAST) | 0x800000u);
    wr(HE_BCAST, (rd(HE_BCAST) & 0xff800fffu) | ((high << 12) & 0x7ff000u));
}

/*
 * The libraries' hal_he_set_uora_parameter, written out: their word 0x600a4c84's bits 25 to 31 given 2^OCWmin - 1 and
 * its bits 18 to 24 given 2^OCWmax - 1, the two from the byte's low three bits and its next three. Their start's byte,
 * 0x2b, holds OCWmin 3 and OCWmax 5, so the fields take 7 and 31. What the register's fields otherwise are is not known.
 */
#define HE_UORA 0x600a4c84u

static void mac_he_set_uora_parameter(const uint8_t *p)
{
    wr(HE_UORA, (rd(HE_UORA) & 0x01ffffffu) | (((1u << (p[0] & 7u)) - 1u) << 25));
    wr(HE_UORA, (rd(HE_UORA) & 0xfe03ffffu) | (((1u << ((p[0] >> 3) & 7u)) - 1u) << 18));
}

/*
 * The libraries' hal_mac_set_rxq_policy, written out: the interface's policy word's two queue bits, RX_POLICY_QUEUE's
 * 0x100 and 2, set for a nonzero argument and cleared for the zero their start passes.
 */
static void mac_rxq_policy(uint32_t iface, uint32_t enable)
{
    uint32_t a = RX_POLICY + 4u * iface;
    if (enable != 0) {
        wr(a, rd(a) | 0x100u);
        wr(a, rd(a) | 0x2u);
    } else {
        wr(a, rd(a) & ~0x100u);
        wr(a, rd(a) & ~0x2u);
    }
}

/*
 * The libraries' hal_mac_set_bssid, written out: the interface's six BSSID bytes into BSSID_LO and the low 16 bits of
 * BSSID_HI, the flag BSSID_FLAG cleared before them and set after.
 */
static void mac_bssid_set(uint32_t iface, const uint8_t *bssid)
{
    uint32_t lo = BSSID_LO + 8u * iface;
    uint32_t hi = BSSID_HI + 8u * iface;
    wr(hi, rd(hi) & ~BSSID_FLAG);
    wr(lo, bssid[0] | (bssid[1] << 8) | (bssid[2] << 16) | ((uint32_t)bssid[3] << 24));
    wr(hi, (rd(hi) & 0xffff0000u) | bssid[4] | ((uint32_t)bssid[5] << 8));
    wr(hi, rd(hi) | BSSID_FLAG);
}

/*
 * The libraries' hal_mac_set_addr, written out: the interface's six address bytes into STA_ADDR_LO and STA_ADDR_HI,
 * the second write covering the whole word, so its high bits clear, then the flag STA_ADDR_FLAG set.
 */
static void mac_addr_set(uint32_t iface, const uint8_t *addr)
{
    uint32_t lo = STA_ADDR_LO + 8u * iface;
    uint32_t hi = STA_ADDR_HI + 8u * iface;
    wr(lo, addr[0] | (addr[1] << 8) | (addr[2] << 16) | ((uint32_t)addr[3] << 24));
    wr(hi, addr[4] | (addr[5] << 8));
    wr(hi, rd(hi) | STA_ADDR_FLAG);
}

/*
 * The libraries' hal_he_clr_multi_bssid, written out: their start's call clears interface 2's queue bits, its BSSID
 * and address and gives it the default policy, which takes their flags away, first; then, on MAC_CONF(1) and
 * MAC_CONF(0), it clears bit 8, sets bits 9 to 16 and clears the low byte, and, on HE_MULTI_BSSID(1) and
 * HE_MULTI_BSSID(0), clears bits 16 to 23; then each of the eight slots' TX_MPLEN, bit 2 cleared.
 * Their read of MAC_CONF(0) first is the guard: if its bit 17 is set, the call returns unwritten.
 */

static void mac_he_clr_multi_bssid(void)
{
    static const uint8_t zero[6];

    if (rd(MAC_CONF(0)) & 0x20000u) {
        return;
    }
    mac_rxq_policy(2u, 0u);
    mac_bssid_set(2u, zero);
    mac_addr_set(2u, zero);
    mac_rx_set_policy(2u, 0);
    wr(MAC_CONF(1), rd(MAC_CONF(1)) & ~0x100u);
    wr(MAC_CONF(1), rd(MAC_CONF(1)) | 0x1fe00u);
    wr(MAC_CONF(1), rd(MAC_CONF(1)) & ~0xffu);
    wr(HE_MULTI_BSSID(1), rd(HE_MULTI_BSSID(1)) & 0xff00ffffu);
    wr(MAC_CONF(0), rd(MAC_CONF(0)) & ~0x100u);
    wr(MAC_CONF(0), rd(MAC_CONF(0)) | 0x1fe00u);
    wr(MAC_CONF(0), rd(MAC_CONF(0)) & ~0xffu);
    wr(HE_MULTI_BSSID(0), rd(HE_MULTI_BSSID(0)) & 0xff00ffffu);
    for (uint32_t a = TX_MPLEN(0); a != TX_MPLEN(TX_SLOTS); a -= TX_QUEUE_STEP) {
        wr(a, rd(a) & ~0x4u);
    }
}

/*
 * The libraries' hal_he_set_bf_report_rate, written out: their beamforming report rate, a byte their two arguments
 * fold into -- enable's two low bits into 0x60, and rate's low byte less 0x10 or, when at most 9 is left after 0x10
 * is taken off, less 0x1a -- and four read-modify-writes that put it into MAC+0x458's bits 21 to 27, 14 to 20, 7 to 13
 * and 0 to 6.
 */
#define HE_BF_REPORT_RATE (MAC_BASE + 0x458u)

static void mac_he_set_bf_report_rate(uint32_t enable, uint32_t rate)
{
    uint32_t v = rate & 0xffu;

    if (enable != 0) {
        uint32_t sub = (rate - 0x10u <= 9u) ? 0x10u : 0x1au;

        v = ((v - sub) & 0xffu) | ((enable << 5) & 0x60u);
    }
    wr(HE_BF_REPORT_RATE, (rd(HE_BF_REPORT_RATE) & 0xf01fffffu) | ((v << 21) & 0x0fe00000u));
    wr(HE_BF_REPORT_RATE, (rd(HE_BF_REPORT_RATE) & 0xffe03fffu) | ((v << 14) & 0x1fc000u));
    wr(HE_BF_REPORT_RATE, (rd(HE_BF_REPORT_RATE) & 0xffffc07fu) | ((v << 7) & 0x3f80u));
    wr(HE_BF_REPORT_RATE, (rd(HE_BF_REPORT_RATE) & ~0x7fu) | (v & 0x7fu));
}

/*
 * The libraries' hal_init_bf, written out: their beamforming group. On 0x600a4c78, six read-modify-writes clear bits
 * 21 and 23, set bit 19, clear bits 8 to 15 and give them 0x7100, clear the low byte and set bit 5, then clear bits 16
 * to 18 and give them 0x50000; bit 31 set on MAC+0x474; bits 12 to 27 cleared and 0x801000 or'd on MAC+0x470;
 * 0x600a4de0's bits 20 to 24, read twice, raised by one; bit 2 cleared and bit 3 set on BF_FEEDBACK; their report-rate
 * call with one and 0x10; and 0x600a7128's low 24 bits kept and 0xd2000000 or'd, its word read once more after.
 * Their body asks the adapter's env_is_chip first, which the board's answers true, so the field's one is the chip's
 * and their other path, three there instead, is not written.
 */
#define HE_HAL_C78  0x600a4c78u
#define HE_MAC_474  (MAC_BASE + 0x474u)
#define HE_MAC_470  (MAC_BASE + 0x470u)
#define HE_HAL_DE0  0x600a4de0u
#define BF_FEEDBACK (MAC_BASE + 0x9cu) /* bit 2 cleared and bit 3 set, as their esp_test_bf_set_feedback sets them */
#define HE_HAL_7128 0x600a7128u

static void mac_init_bf(void)
{
    uint32_t base, field;

    wr(HE_HAL_C78, rd(HE_HAL_C78) & ~0x200000u);
    wr(HE_HAL_C78, rd(HE_HAL_C78) & ~0x800000u);
    wr(HE_HAL_C78, rd(HE_HAL_C78) | 0x80000u);
    wr(HE_HAL_C78, (rd(HE_HAL_C78) & ~0xff00u) | 0x7100u);
    wr(HE_HAL_C78, (rd(HE_HAL_C78) & ~0xffu) | 0x20u);
    wr(HE_HAL_C78, (rd(HE_HAL_C78) & 0xfff8ffffu) | 0x50000u);
    wr(HE_MAC_474, rd(HE_MAC_474) | 0x80000000u);
    wr(HE_MAC_470, (rd(HE_MAC_470) & 0xff000fffu) | 0x801000u);

    base = rd(HE_HAL_DE0);
    field = ((rd(HE_HAL_DE0) >> 20) & 0x1fu) + 1u;
    wr(HE_HAL_DE0, (base & ~0x1f00000u) | ((field << 20) & 0x1f00000u));

    wr(BF_FEEDBACK, rd(BF_FEEDBACK) & ~0x4u);
    wr(BF_FEEDBACK, rd(BF_FEEDBACK) | 0x8u);
    mac_he_set_bf_report_rate(1u, 0x10u);
    wr(HE_HAL_7128, (rd(HE_HAL_7128) & 0x00ffffffu) | 0xd2000000u);
    (void)rd(HE_HAL_7128); /* their body's last read, which it makes and does not use */
}

/*
 * The libraries' hal_init_tb_tx, written out: their trigger-based TX word 0x600a4df8, bit 15 cleared and then bit 14.
 */
#define HE_TB_TX 0x600a4df8u

static void mac_init_tb_tx(void)
{
    wr(HE_TB_TX, rd(HE_TB_TX) & ~0x8000u);
    wr(HE_TB_TX, rd(HE_TB_TX) & ~0x4000u);
}

/*
 * The libraries' hal_set_tx_min_pwr, written out: HE_TX_MIN's bits 4 to 9 given the argument's low six bits raised
 * by four, -11 for the start's call giving 0x350.
 */
static void mac_set_tx_min_pwr(int pwr)
{
    wr(HE_TX_MIN, (rd(HE_TX_MIN) & ~0x3f0u) | (((uint32_t)pwr << 4) & 0x3f0u));
}

/*
 * The libraries' hal_init_tx_pwr, written out: their power table, one two-byte entry for each of their 43 rates,
 * filled by their phy_get_max_pwr, and then their hal_init_tb_power and hal_init_imrsp_power, both below.
 * The table is the driver's own; the one their net80211 paths read is theirs, filled by their hal_init_tx_pwr at
 * the end of the driver's start, so the two hold the same values.
 * phy_get_max_pwr stays a libphy call, and the host replay stubs it with the values of the recorded run.
 */
static uint8_t mac_pwr_table[0x2b * 2u];

extern void phy_get_max_pwr(uint32_t index, uint8_t out[2]);

/* The libraries' hal_get_tx_pwr, written out: a table entry, the rates above 0x19 read ten back. */
static uint8_t mac_get_tx_pwr(uint32_t index)
{
    return mac_pwr_table[2u * (index > 0x19u ? index - 0xau : index)];
}

/*
 * The libraries' hal_init_tb_power, written out: three of their power words, MAC+0x430, MAC+0x434 and MAC+0x438,
 * each given a six-bit field of a table entry at a time -- rates 0x10 to 0x19 from the table, the last two through
 * phy_get_max_pwr. What the fields mean is not known.
 */
#define HE_TB_POWER0 (MAC_BASE + 0x430u)
#define HE_TB_POWER1 (MAC_BASE + 0x434u)
#define HE_TB_POWER2 (MAC_BASE + 0x438u)

static void mac_init_tb_power(void)
{
    uint8_t v[2];

    wr(HE_TB_POWER0, (rd(HE_TB_POWER0) & ~0x3fu) | (mac_get_tx_pwr(0x10u) & 0x3fu));
    wr(HE_TB_POWER0, (rd(HE_TB_POWER0) & ~0x3f00u) | (((uint32_t)mac_get_tx_pwr(0x11u) << 8) & 0x3f00u));
    wr(HE_TB_POWER0, (rd(HE_TB_POWER0) & ~0x3f0000u) | (((uint32_t)mac_get_tx_pwr(0x12u) << 16) & 0x3f0000u));
    wr(HE_TB_POWER0, (rd(HE_TB_POWER0) & ~0x3f000000u) | (((uint32_t)mac_get_tx_pwr(0x13u) << 24) & 0x3f000000u));
    wr(HE_TB_POWER1, (rd(HE_TB_POWER1) & ~0x3fu) | (mac_get_tx_pwr(0x14u) & 0x3fu));
    wr(HE_TB_POWER1, (rd(HE_TB_POWER1) & ~0x3f00u) | (((uint32_t)mac_get_tx_pwr(0x15u) << 8) & 0x3f00u));
    wr(HE_TB_POWER1, (rd(HE_TB_POWER1) & ~0x3f0000u) | (((uint32_t)mac_get_tx_pwr(0x16u) << 16) & 0x3f0000u));
    wr(HE_TB_POWER1, (rd(HE_TB_POWER1) & ~0x3f000000u) | (((uint32_t)mac_get_tx_pwr(0x17u) << 24) & 0x3f000000u));
    wr(HE_TB_POWER2, (rd(HE_TB_POWER2) & ~0x3fu) | (mac_get_tx_pwr(0x18u) & 0x3fu));
    wr(HE_TB_POWER2, (rd(HE_TB_POWER2) & ~0x3f00u) | (((uint32_t)mac_get_tx_pwr(0x19u) << 8) & 0x3f00u));
    phy_get_max_pwr(0x1au, v);
    wr(HE_TB_POWER2, (rd(HE_TB_POWER2) & ~0x3f0000u) | (((uint32_t)v[0] << 16) & 0x3f0000u));
    phy_get_max_pwr(0x1bu, v);
    wr(HE_TB_POWER2, (rd(HE_TB_POWER2) & ~0x3f000000u) | (((uint32_t)v[0] << 24) & 0x3f000000u));
}

/*
 * The libraries' hal_init_imrsp_power, written out: ten of their power words, MAC+0x408 on, 4 apart, each with three
 * read-modify-writes -- bits 22 and 23 cleared, bit 23 set again from the seventh word on; bits 16 to 21 cleared and
 * given one of their constants; and bits 8 to 13 given a table entry. Their name says the implicit response's power.
 */
static void mac_init_imrsp_power(void)
{
    static const struct {
        uint8_t high, rate; /* the constant for bits 16 to 21, and the rate whose entry goes to bits 8 to 13 */
    } words[10] = {
        {0x00, 0x00}, {0x01, 0x00}, {0x05, 0x05}, {0x0b, 0x0b}, {0x0a, 0x0a},
        {0x09, 0x09}, {0x10, 0x10}, {0x11, 0x11}, {0x12, 0x12}, {0x12, 0x12},
    };

    for (uint32_t i = 0; i < 10u; i++) {
        uint32_t a = MAC_BASE + 0x408u + 4u * i;

        wr(a, (rd(a) & ~0xc00000u) | (i >= 6u ? 0x800000u : 0u));
        wr(a, (rd(a) & ~0x3f0000u) | ((uint32_t)words[i].high << 16));
        wr(a, (rd(a) & ~0x3f00u) | (((uint32_t)mac_get_tx_pwr(words[i].rate) << 8) & 0x3f00u));
    }
}

static void mac_init_tx_pwr(void)
{
    for (uint32_t i = 0; i < 0x2bu; i++) {
        phy_get_max_pwr(i, &mac_pwr_table[2u * i]);
    }
    mac_init_tb_power();
    mac_init_imrsp_power();
}

void mac_he_init(void)
{
    wr(HE_CTRL, rd(HE_CTRL) & ~0x80000000u);
    wr(HE_CTRL, rd(HE_CTRL) & ~0xc0000000u);
    mac_init_bf();
    wr(HE_MAC_10C, (rd(HE_MAC_10C) & ~0xc0000u) | 0x80000u);
    wr(COLOR_BITMAP, (rd(COLOR_BITMAP) & ~0xfcu) | 0xf0u);
    wr(COLOR_ISR, rd(COLOR_ISR) & ~0x1000u);
    mac_init_tb_tx();
    mac_init_tx_pwr();
    mac_he_set_ersu(0);
    mac_set_tx_min_pwr(-0xb);
    wr(HE_CTRL, (rd(HE_CTRL) & ~0xff8u) | 0xbe0u);
    for (uint32_t a = HE_TABLE; a != HE_TABLE_END; a += 4u) {
        wr(a, 0);
    }
    wr(TX_CONF0(0), rd(TX_CONF0(0)) & ~0xc0000000u);
    wr(TX_CONF0(1), rd(TX_CONF0(1)) & ~0xc0000000u);
    wr(TX_CONF0(2), rd(TX_CONF0(2)) & ~0xc0000000u);
    wr(TX_CONF0(3), rd(TX_CONF0(3)) & ~0xc0000000u);
    wr(TXRX_HAL_C98, rd(TXRX_HAL_C98) | 0x4u);
    wr(HE_HAL_CBC, rd(HE_HAL_CBC) | 0x80000000u);
    wr(HE_HAL_C88, rd(HE_HAL_C88) | 0x2u);
    wr(HE_HAL_C88, rd(HE_HAL_C88) | 0x1u);
    wr(BEACON_CRC, (rd(BEACON_CRC) & 0x3fffffffu) | 0x40000000u);
    /* The byte their start passed: the UORA contention window, as 802.11ax's OCW Range lays it out, OCWmin 3 in
       bits 0 to 2 and OCWmax 5 in bits 3 to 5; their call reads it on the spot, so a local carries it. */
    uint8_t uora = 0x2b;
    mac_he_set_bcast_ru(0x7fd, 0, 0);
    mac_he_set_uora_parameter(&uora);
    wr(HE_TX_MIN, rd(HE_TX_MIN) | 0x20000u);
    wr(MAC_CONF(0), rd(MAC_CONF(0)) & ~0x100u);
    wr(MAC_CONF(0), rd(MAC_CONF(0)) & ~0x20000u);
    mac_he_clr_multi_bssid();
    mac_he_set_co_hosted_bss(0, 0);
}

/*
 * The libraries' mac_last_rxbuf_init, written out: six rules that match a frame's payload,
 * each a control word, a value and a mask, and then the word before the table and RX_WORD given their bits.
 * The control word's low byte is the offset the value is matched at, and the values are the frame's bytes read
 * little-endian, so the register holds each reversed; the rows below name what they read.
 * The third value, 0x0808, reads as an EtherType too but is not known, nor are the rest of the control word,
 * the two six-bit groups of the word before the table (named for the six rules it holds a bit for, a guess),
 * or what the match is for.
 */
#define RX_MATCH_CTRL(i)  (MAC_BASE + 0x120u + 4u * (i))
#define RX_MATCH_VALUE(i) (MAC_BASE + 0x13cu + 4u * (i))
#define RX_MATCH_MASK(i)  (MAC_BASE + 0x158u + 4u * (i))
#define RX_MATCH_ENABLE   (MAC_BASE + 0x11cu)

void mac_rx_match_init(void)
{
    static const struct {
        uint32_t ctrl, value, mask;
    } rules[6] = {
        {0x00023006u, 0x00000608u, 0x0000ffffu}, /* at 6, 08 06: ARP */
        {0x00023006u, 0x00000808u, 0x0000ffffu}, /* at 6, 08 08: not known */
        {0x00023006u, 0x00008e88u, 0x0000ffffu}, /* at 6, 88 8e: EAPOL */
        {0x0002301cu, 0x44004300u, 0xffffffffu}, /* at 28, 00 43 00 44: DHCP, 67 to 68 */
        {0x0002301cu, 0x43004400u, 0xffffffffu}, /* at 28, 00 44 00 43: DHCP, 68 to 67 */
        {0x00023011u, 0x00000001u, 0x000000ffu}, /* at 17, 01: ICMP */
    };
    for (uint32_t i = 0; i < 6u; i++) {
        wr(RX_MATCH_CTRL(i), rules[i].ctrl);
        wr(RX_MATCH_VALUE(i), rules[i].value);
        wr(RX_MATCH_MASK(i), rules[i].mask);
    }
    wr(RX_MATCH_ENABLE, rd(RX_MATCH_ENABLE) | 0x3f00u);
    wr(RX_MATCH_ENABLE, rd(RX_MATCH_ENABLE) | 0x7eu);
    wr(RX_WORD, rd(RX_WORD) | 0x08000000u);
}

/*
 * The libraries' hal_mac_disable_low_rate, written out: the ROM's phy_disable_low_rate first, then their four
 * low-rate registers, 0x90a0b and 0x50100 written twice each, and their hal_he_set_bbrxhung_time(0).
 * Their hal_mac_rate_autoack_init, which hal_init calls just before it, is an empty return; the driver's start drops it.
 */
void mac_low_rate_disable(void)
{
    phy_disable_low_rate();
    wr(RATE_CTS0, 0x90a0bu);
    wr(RATE_CTS1, 0x50100u);
    wr(RATE_ACK0, 0x90a0bu);
    wr(RATE_ACK1, 0x50100u);
    mac_he_set_bbrxhung_time(0);
}

void mac_config_finish(void)
{
    wr(HAL_HOLD, 0x19a879e0u);
    wr(HAL_WORD, rd(HAL_WORD) | 0x10000000u);
    wr(RX_WORD, (rd(RX_WORD) & 0xffffff00u) | 1u);
    wr(RX_WORD, (rd(RX_WORD) & 0xffff00ffu) | 0x100u);
}

/*
 * The libraries' hal_mac_set_rxbuf_reload_use_hw_beacon_enable, written out: RX_CTRL's bit 27,
 * which their name gives as the receive buffer's reload by the hardware beacon.
 */
void mac_rx_reload_hw_beacon(void)
{
    wr(RX_CTRL, rd(RX_CTRL) | RX_CTRL_HW_BEACON_RELOAD);
}

/*
 * The libraries' hal_timer_update_by_rtc, written out: 0x600ad030's bit 27 set, or cleared for a which of zero,
 * which the driver's call, always 1, never takes; and 0x600ad070's low 18 bits given the hz the call passed,
 * the slow clock's period in Q13.19 microseconds, which the adapter's slowclk_cal_get gives.
 * The 18-bit mask is the disassembly's: it keeps a Q13.19 value's fraction alone, so what the field holds is not
 * known.
 * Nor is what the timer is, beyond their name, in the block mac.c's TSF_CTRL and PWR_STATUS sit.
 */
#define TIMER_30 0x600ad030u
#define TIMER_70 0x600ad070u

void mac_timer_update_by_rtc(uint32_t which, uint32_t hz)
{
    if (which != 0) {
        wr(TIMER_30, rd(TIMER_30) | 0x08000000u);
        wr(TIMER_70, (rd(TIMER_70) & 0xfffc0000u) | (hz & 0x3ffffu));
    } else {
        wr(TIMER_30, rd(TIMER_30) & ~0x08000000u);
    }
}

/*
 * The libraries' hal_crypto_init, written out:
 * their cipher's two configuration words, each 0x30000, and the three words after them cleared.
 * Their engine word, 0x30103, which their hal_crypto_enable writes into the first, stays unset:
 * with it set the access point took none of the station's sending; see NOTES.md.
 */
#define CRYPTO_BASE 0x600a4800u
void mac_crypto_init(void)
{
    wr(CRYPTO_BASE + 0x00u, 0x30000u);
    wr(CRYPTO_BASE + 0x04u, 0x30000u);
    wr(CRYPTO_BASE + 0x08u, 0);
    wr(CRYPTO_BASE + 0x0cu, 0);
    wr(CRYPTO_BASE + 0x10u, 0);
}

/*
 * The libraries' hal_attenna_init (their spelling), written out: each of the eight slots' TX_RESP, slot 0 down to
 * slot 7, cleared of bits 0 to 4 and set at bit 5, in their two passes -- 0 to 2 cleared first, then 3 cleared, 5 set
 * and 4 cleared -- and the single word ANTENNA_MAC_2CC, cleared of 0 to 2 and set at 5. What ANTENNA_MAC_2CC holds
 * is not known; each access is one here, in their order, since the trace holds each.
 */
#define ANTENNA_MAC_2CC  (MAC_BASE + 0x2ccu)

void mac_antenna_init(void)
{
    for (uint32_t a = TX_RESP(0); a >= TX_RESP(TX_SLOTS - 1); a -= TX_PPDU_STEP) {
        wr(a, rd(a) & ~0x7u);
    }
    for (uint32_t a = TX_RESP(0); a >= TX_RESP(TX_SLOTS - 1); a -= TX_PPDU_STEP) {
        wr(a, rd(a) & ~0x8u);
        wr(a, rd(a) | 0x20u);
        wr(a, rd(a) & ~0x10u);
    }
    wr(ANTENNA_MAC_2CC, rd(ANTENNA_MAC_2CC) & ~0x7u);
    wr(ANTENNA_MAC_2CC, rd(ANTENNA_MAC_2CC) | 0x20u);
}

/*
 * The libraries' coex PTI, the packet-type map their hal_set_*_pti write into PTI_DEFAULT and PTI_RX (see mac_stop),
 * written out for the four whose value the driver's start supplies: their hal_coex_pti_init sets PTI_DEFAULT's
 * fifth bit, their hal_set_rx_active_pti and hal_set_wifi_default_pti the low nibble -- the active PTI and the
 * default one their start reads through the adapter's coex_pti_get -- and their hal_set_rx_ack_pti the second
 * nibble. Their hal_set_ofdma_sequence_pti and hal_timer_update_by_rtc, whose groups the driver still calls, write
 * the words those two name.
 */
void mac_coex_pti_init(void)
{
    wr(PTI_DEFAULT, rd(PTI_DEFAULT) | 0x20u);
}

void mac_rx_active_pti(uint32_t pti)
{
    wr(PTI_RX, (rd(PTI_RX) & ~0xfu) | (pti & 0xfu));
}

void mac_rx_ack_pti(uint32_t pti)
{
    wr(PTI_RX, (rd(PTI_RX) & ~0xf0u) | ((pti << 4) & 0xffu));
}

void mac_wifi_default_pti(uint32_t pti)
{
    wr(PTI_DEFAULT, (rd(PTI_DEFAULT) & ~0xfu) | (pti & 0xfu));
}

/*
 * The libraries' OFDMA sequence PTI, their hal_set_ofdma_sequence_pti with its leaves hal_set_tb_pti,
 * hal_set_beamf_pti and hal_set_beamf_mt_pti, written out: the twelve bytes their body reads through the adapter's
 * coex_pti_get reach the two words 0x600a4dd0 and 0x600a4dd4, one nibble each, the leaves' read-modify-writes in
 * their order. Their body then reads the two words for its log; the own makes the reads too and has no log of them.
 * A leaf is one read-modify-write per nibble, since the trace holds each access.
 */
#define OFDMA_PTI_W0 0x600a4dd0u
#define OFDMA_PTI_W1 0x600a4dd4u

/* Their hal_set_tb_pti: v0 to the first word's [27:24], v1 to [23:20], v2 to [19:16], v3 to [15:12], v5 to [11:8],
   v6 to [7:4] and v4 to [3:0]. */
static void mac_tb_pti(uint32_t v0, uint32_t v1, uint32_t v2, uint32_t v3, uint32_t v4, uint32_t v5, uint32_t v6)
{
    wr(OFDMA_PTI_W0, (rd(OFDMA_PTI_W0) & 0xf0ffffffu) | ((v0 << 24) & 0x0f000000u));
    wr(OFDMA_PTI_W0, (rd(OFDMA_PTI_W0) & 0xff0fffffu) | ((v1 << 20) & 0x00f00000u));
    wr(OFDMA_PTI_W0, (rd(OFDMA_PTI_W0) & 0xfff0ffffu) | ((v2 << 16) & 0x000f0000u));
    wr(OFDMA_PTI_W0, (rd(OFDMA_PTI_W0) & 0xffff0fffu) | ((v3 << 12) & 0x0000f000u));
    wr(OFDMA_PTI_W0, (rd(OFDMA_PTI_W0) & 0xfffff0ffu) | ((v5 << 8) & 0x00000f00u));
    wr(OFDMA_PTI_W0, (rd(OFDMA_PTI_W0) & 0xffffff0fu) | ((v6 << 4) & 0x000000f0u));
    wr(OFDMA_PTI_W0, (rd(OFDMA_PTI_W0) & 0xfffffff0u) | (v4 & 0x0000000fu));
}

/* Their hal_set_beamf_pti: v7 to the first word's [31:28], and v8 to the second's [7:4] and v9 to its [3:0]. */
static void mac_beamf_pti(uint32_t v7, uint32_t v8, uint32_t v9)
{
    wr(OFDMA_PTI_W0, (rd(OFDMA_PTI_W0) & 0x0fffffffu) | (v7 << 28));
    wr(OFDMA_PTI_W1, (rd(OFDMA_PTI_W1) & 0xffffff0fu) | ((v8 << 4) & 0x000000f0u));
    wr(OFDMA_PTI_W1, (rd(OFDMA_PTI_W1) & 0xfffffff0u) | (v9 & 0x0000000fu));
}

/* Their hal_set_beamf_mt_pti: v11 to the second word's [11:8] and v10 to its [15:12] -- the order the disassembly
   gives, since the trace cannot settle the two, both bytes being 3. */
static void mac_beamf_mt_pti(uint32_t v10, uint32_t v11)
{
    wr(OFDMA_PTI_W1, (rd(OFDMA_PTI_W1) & 0xfffff0ffu) | ((v11 << 8) & 0x00000f00u));
    wr(OFDMA_PTI_W1, (rd(OFDMA_PTI_W1) & 0xffff0fffu) | ((v10 << 12) & 0x0000f000u));
}

void mac_ofdma_sequence_pti(const uint8_t pti[12])
{
    mac_tb_pti(pti[0], pti[1], pti[2], pti[3], pti[4], pti[5], pti[6]);
    mac_beamf_pti(pti[7], pti[8], pti[9]);
    mac_beamf_mt_pti(pti[10], pti[11]);
    (void)rd(OFDMA_PTI_W0); /* their body's two reads, for its log */
    (void)rd(OFDMA_PTI_W1);
}

/*
 * The libraries' wifi_mode_set and _do_wifi_start, written out for the station's fresh start, so that their two
 * entry points go: their wifi_mode_set's low-rate disable, then their _do_wifi_start's wifi_station_start --
 * the STA's TSF on, interface 0's address and its access point's, its receive policy with its own flags set and
 * its queue policy left clear, the receive enabled, and their cipher's own entry cleared -- with their STA_START
 * posted through the adapter, which main.c waits on. Only the hardware's accesses are here: none of net80211's own
 * state, its control block's vif words and statics, is written, and the ordinary runs hold that it is not wanted.
 * See NOTES.md.
 */
void mac_station_start(const uint8_t sta[6], void (*sta_start)(void))
{
    mac_low_rate_disable();
    wr(TSF_CTRL, rd(TSF_CTRL) | 0x88000000u);
    wr(TSF_CTRL, (rd(TSF_CTRL) & 0xff87ffffu) | 0x00080000u);
    mac_addr_set(0, sta);
    mac_bssid_set(0, sta);
    mac_rx_set_policy(0, 1);
    mac_rxq_policy(0, 0);
    wr(RX_CTRL, rd(RX_CTRL) | RX_CTRL_ENABLE);
    wr(CRYPTO_BASE, 0x30000u);
    /* their wDev_Crypto_Disable passes hal_crypto_disable the mask their wdev wrote for the interface;
       the word reads 0 in the start, so the write returns what it read. */
    wr(KEY_VALID, rd(KEY_VALID));
    sta_start();
}

/* The adapter's trace, the driver's own, which names the segments between the groups; declared here rather than
   through osi.h, which pulls what the host does not build. The replay test leaves it a no-op. */
extern void osi_trace(const char *what, uint32_t a0, uint32_t a1);

/* The OFDMA sequence's twelve PTI events, in their hal_set_ofdma_sequence_pti's order, which its body reads through
   the adapter's coex_pti_get: the map's event 1, 3 and 0xa, which gave 5, 7 and 3 in the recorded run. */
static const uint8_t ofdma_pti_events[12] = {1, 3, 3, 3, 1, 1, 1, 1, 3, 3, 0xau, 0xau};

/*
 * The libraries' hal_init, the MAC's configuration, written out whole:
 * its own register writes about HAL_CFG and HAL_HOLD and its receive-policy words, and its groups in its order,
 * each named through osi_trace so that a traced run's segments show its accesses. See NOTES.md.
 * The driver's start and the host's replay both call this, so neither keeps the order of its own;
 * the two adapter values it needs, the slow clock's period and the coex PTI bytes, come through the pointers.
 */
void mac_config(uint32_t (*slowclk_cal_get)(void), int (*coex_pti_get)(uint32_t, uint8_t *), uint32_t rx_base)
{
    osi_trace("mac-config", 0, 0);
    mac_config_start();
    osi_trace("mac-txrx", 0, 0);
    mac_queues_init();
    osi_trace("mac-policy", 0, 0);
    for (uint32_t i = 0; i < 4u; i++) {
        if (i == 1u) {
            continue; /* interface 1, the soft AP's, which the driver never brings up; see NOTES.md */
        }
        mac_rx_policy_word(i);
        mac_rx_set_policy(i, 0);
    }
    osi_trace("mac-rxbuf", 0, 0);
    mac_rx_base_init(rx_base);
    osi_trace("mac-he", 0, 0);
    mac_he_init();
    mac_rx_match_init();
    osi_trace("mac-rate", 0, 0);
    mac_low_rate_disable();
    osi_trace("mac-crypto", 0, 0);
    mac_crypto_init();
    osi_trace("mac-antenna", 0, 0);
    mac_antenna_init();
    osi_trace("mac-post", 0, 0);
    mac_config_finish();
    mac_rx_reload_hw_beacon();
    osi_trace("mac-pti", 0, 0);
    mac_timer_update_by_rtc(1, slowclk_cal_get());
    mac_coex_pti_init();
    uint8_t pti_active = 0, pti_default = 1; /* their hal_init's own bytes, which their coex_pti_get fills */
    coex_pti_get(3, &pti_active);
    coex_pti_get(0xfu, &pti_default);
    mac_rx_active_pti(0);
    mac_rx_ack_pti(pti_active);
    mac_wifi_default_pti(pti_default);
    uint8_t ofdma[12]; /* the OFDMA sequence's twelve PTI bytes, which their coex_pti_get fills */
    for (uint32_t i = 0; i < 12u; i++) {
        coex_pti_get(ofdma_pti_events[i], &ofdma[i]);
    }
    mac_ofdma_sequence_pti(ofdma);
}
