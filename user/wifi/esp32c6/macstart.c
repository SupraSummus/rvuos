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
 * their HAL_CFG start and the wait for it, and HAL_HOLD and HAL_MISC cleared.
 * Then, after the groups, HAL_HOLD, HAL_WORD and RX_WORD as their hal_init leaves them.
 * The driver's own start makes these, and mac_config below runs them and the groups in the libraries' order.
 */
void mac_config_start(void)
{
    wr(HAL_CFG, rd(HAL_CFG) | 2u);
    while (!(rd(HAL_CFG) & 1u)) {
    }
    wr(HAL_HOLD, 0);
    wr(HAL_MISC, 0xffffffffu);
}

/*
 * The libraries' mac_txrx_init, written out:
 * its sniffers cleared and set to their default, the words that set up its queues and hold the MAC,
 * the receive left disabled, which mac_rx_on sets again, and the three HE calls at its middle.
 * The sniffers, HAL_WORD, HAL_CTRL and RX_CTRL are named in macregs.h; the others carry the offset they were reached
 * at, since what they are is not known.
 * Each of their read-modify-writes is one here, in their order, since the trace holds each access.
 */
#define TXRX_MAC_100  (MAC_BASE + 0x100u)
#define TXRX_MAC_110  (MAC_BASE + 0x110u)
#define TXRX_MAC_114  (MAC_BASE + 0x114u)
#define TXRX_HAL_C1C  0x600a4c1cu
#define TXRX_HAL_C20  0x600a4c20u
#define TXRX_HAL_C24  0x600a4c24u
#define TXRX_HAL_C60  0x600a4c60u
#define TXRX_HAL_C98  0x600a4c98u
#define TXRX_HAL_C9C  0x600a4c9cu
#define TXRX_HAL_CA4  0x600a4ca4u

/* The three HE calls mac_txrx_init makes; they stay theirs, leaves with device writes of their own. */
extern void hal_he_set_mac_delay(uint32_t);
extern void hal_he_set_ack_rate(uint32_t);
extern void hal_he_set_bbrxhung_time(uint32_t);

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
    wr(TXRX_MAC_100, rd(TXRX_MAC_100) & 0xffffu);
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

    /* Their first call passes a register's address left in a0,
       which hal_he_set_mac_delay tests only against zero, so the driver passes 1;
       the two after it pass zero. */
    hal_he_set_mac_delay(1);
    hal_he_set_ack_rate(0);
    hal_he_set_bbrxhung_time(0);

    wr(TXRX_HAL_C1C, rd(TXRX_HAL_C1C) | 0x80000000u);
    wr(TXRX_HAL_C1C, rd(TXRX_HAL_C1C) | 0x40000000u);
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
 * Their function takes three flags beyond the interface, zero here, and touches nothing for interface 3;
 * their station's own call, hal_mac_rx_set_policy(0, 1, 1, 1), is mac_receive's.
 */
void mac_rx_set_policy(uint32_t iface)
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
    wr(w, rd(w) & ~BSSID_FLAG);
    wr(a, rd(a) & ~STA_ADDR_FLAG);
}

/*
 * The libraries' mac_rxbuf_init, written out: the low byte of the word before RX_CTRL cleared, 0x600a407c,
 * whose other bits are not known, and the receive base pointed at their own control block, wDevCtrl,
 * the descriptor list their receive fills, which the driver's own list replaces once it takes the MAC; see mac.c.
 */
#define RXBUF_MAC_7C (MAC_BASE + 0x07cu)

extern uint32_t wDevCtrl; /* the libraries' receive control block, whose first word their receive base takes */

void mac_rx_base_init(void)
{
    wr(RXBUF_MAC_7C, rd(RXBUF_MAC_7C) & 0xffffff00u);
    wr(RX_BASE, wDevCtrl);
}

/*
 * The libraries' hal_he_init, written out: their hal_init's 802.11ax (HE) group, the words it writes itself and the
 * HE calls it leaves, in its order. The calls' names give the mechanisms -- beamforming, trigger-based TX and its
 * power tables, ER SU, the transmitting minimum power, the broadcast RU, the UORA contention window, multi-BSSID and
 * the co-hosted BSS mask -- and each stays their call, with the argument their start passed.
 * Of the words it writes itself, the two that read as something are their table at 0x600a55f0, cleared, and the
 * transmitting minimum power's own word, 0x600a4400, whose bits 4 to 9 their minimum-power call sets to -11 and
 * where this group ors bit 17; the rest carry the offset they were reached at, since which bit is which is not known.
 */
#define HE_CTRL     0x600a4c80u /* cleared before the calls and set after them, whatever it gates */
#define HE_MAC_10C  (MAC_BASE + 0x10cu)
#define HE_MAC_48   (MAC_BASE + 0x48u)
#define HE_HAL_C2C  0x600a4c2cu
#define HE_HAL_C88  0x600a4c88u
#define HE_HAL_CBC  0x600a4cbcu
#define HE_HAL_D30  0x600a4d30u
#define HE_HAL_D40  0x600a4d40u
#define HE_HAL_D50  0x600a4d50u
#define HE_HAL_D60  0x600a4d60u
#define HE_MAC_20   (MAC_BASE + 0x20u) /* the multi-BSSID and co-hosted BSS control, which their two calls write too */
#define HE_MAC_2D4  (MAC_BASE + 0x2d4u)
#define HE_TX_MIN   (MAC_BASE + 0x400u)
#define HE_TABLE    0x600a55f0u /* their table, cleared */
#define HE_TABLE_END 0x600a57d0u

/* The HE calls hal_he_init makes; they stay theirs, leaves with device writes of their own. */
extern void hal_init_bf(void);
extern void hal_init_tb_tx(void);
extern void hal_init_tx_pwr(void);
extern void hal_he_set_ersu(uint32_t);
extern void hal_set_tx_min_pwr(int);
extern void hal_he_set_bcast_ru(uint32_t, uint32_t, uint32_t);
extern void hal_he_set_uora_parameter(uint8_t *);
extern void hal_he_clr_multi_bssid(void);
extern void hal_he_set_co_hosted_bss(uint32_t, uint32_t);

void mac_he_init(void)
{
    wr(HE_CTRL, rd(HE_CTRL) & ~0x80000000u);
    wr(HE_CTRL, rd(HE_CTRL) & ~0xc0000000u);
    hal_init_bf();
    wr(HE_MAC_10C, (rd(HE_MAC_10C) & ~0xc0000u) | 0x80000u);
    wr(HE_MAC_48, (rd(HE_MAC_48) & ~0xfcu) | 0xf0u);
    wr(HE_HAL_C2C, rd(HE_HAL_C2C) & ~0x1000u);
    hal_init_tb_tx();
    hal_init_tx_pwr();
    hal_he_set_ersu(0);
    hal_set_tx_min_pwr(-0xb);
    wr(HE_CTRL, (rd(HE_CTRL) & ~0xff8u) | 0xbe0u);
    for (uint32_t a = HE_TABLE; a != HE_TABLE_END; a += 4u) {
        wr(a, 0);
    }
    wr(HE_HAL_D60, rd(HE_HAL_D60) & ~0xc0000000u);
    wr(HE_HAL_D50, rd(HE_HAL_D50) & ~0xc0000000u);
    wr(HE_HAL_D40, rd(HE_HAL_D40) & ~0xc0000000u);
    wr(HE_HAL_D30, rd(HE_HAL_D30) & ~0xc0000000u);
    wr(TXRX_HAL_C98, rd(TXRX_HAL_C98) | 0x4u);
    wr(HE_HAL_CBC, rd(HE_HAL_CBC) | 0x80000000u);
    wr(HE_HAL_C88, rd(HE_HAL_C88) | 0x2u);
    wr(HE_HAL_C88, rd(HE_HAL_C88) | 0x1u);
    wr(HE_MAC_2D4, (rd(HE_MAC_2D4) & 0x3fffffffu) | 0x40000000u);
    /* The byte their start passed: the UORA contention window, as 802.11ax's OCW Range lays it out, OCWmin 3 in
       bits 0 to 2 and OCWmax 5 in bits 3 to 5; their call reads it on the spot, so a local carries it. */
    uint8_t uora = 0x2b;
    hal_he_set_bcast_ru(0x7fd, 0, 0);
    hal_he_set_uora_parameter(&uora);
    wr(HE_TX_MIN, rd(HE_TX_MIN) | 0x20000u);
    wr(HE_MAC_20, rd(HE_MAC_20) & ~0x100u);
    wr(HE_MAC_20, rd(HE_MAC_20) & ~0x20000u);
    hal_he_clr_multi_bssid();
    hal_he_set_co_hosted_bss(0, 0);
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
#define RATE_MAC_440 (MAC_BASE + 0x440u)
#define RATE_MAC_444 (MAC_BASE + 0x444u)
#define RATE_MAC_44C (MAC_BASE + 0x44cu)
#define RATE_MAC_450 (MAC_BASE + 0x450u)

void mac_low_rate_disable(void)
{
    phy_disable_low_rate();
    wr(RATE_MAC_44C, 0x90a0bu);
    wr(RATE_MAC_450, 0x50100u);
    wr(RATE_MAC_440, 0x90a0bu);
    wr(RATE_MAC_444, 0x50100u);
    hal_he_set_bbrxhung_time(0);
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
 * The libraries' hal_attenna_init (their spelling), written out: their eight words 0x74 apart, from 0x600a54bc down
 * to 0x600a5190, each cleared of bits 0 to 4 and set at bit 5, in their two passes -- 0 to 2 cleared first, then 3
 * cleared, 5 set and 4 cleared -- and the single word at 0x600a42cc, cleared of 0 to 2 and set at 5. What the words
 * are is not known; each access is one here, in their order, since the trace holds each.
 */
#define ANTENNA_MAC_LO   (MAC_BASE + 0x1190u)
#define ANTENNA_MAC_HI   (MAC_BASE + 0x14bcu)
#define ANTENNA_MAC_STEP 0x74u
#define ANTENNA_MAC_2CC  (MAC_BASE + 0x2ccu)

void mac_antenna_init(void)
{
    for (uint32_t a = ANTENNA_MAC_HI; a >= ANTENNA_MAC_LO; a -= ANTENNA_MAC_STEP) {
        wr(a, rd(a) & ~0x7u);
    }
    for (uint32_t a = ANTENNA_MAC_HI; a >= ANTENNA_MAC_LO; a -= ANTENNA_MAC_STEP) {
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

/* The adapter's trace, the driver's own, which names the segments between the groups; declared here rather than
   through osi.h, which pulls what the host does not build. The replay test leaves it a no-op. */
extern void osi_trace(const char *what, uint32_t a0, uint32_t a1);

/* The OFDMA sequence's twelve PTI events, in their hal_set_ofdma_sequence_pti's order, which its body reads through
   the adapter's coex_pti_get: the map's event 1, 3 and 0xa, which gave 5, 7 and 3 in the recorded run. */
static const uint8_t ofdma_pti_events[12] = {1, 3, 3, 3, 1, 1, 1, 1, 3, 3, 0xau, 0xau};

/*
 * The libraries' hal_init, the MAC's configuration, written out whole:
 * its own register writes about HAL_CFG and HAL_HOLD and its receive-policy words, and its groups in its order,
 * each named through osi_trace so that a traced run's segments show its accesses. See user/wifi/NOTES.md.
 * The driver's start and the host's replay both call this, so neither keeps the order of its own;
 * the two adapter values it needs, the slow clock's period and the coex PTI bytes, come through the pointers.
 */
void mac_config(uint32_t (*slowclk_cal_get)(void), int (*coex_pti_get)(uint32_t, uint8_t *))
{
    osi_trace("mac-config", 0, 0);
    mac_config_start();
    osi_trace("mac-txrx", 0, 0);
    mac_queues_init();
    osi_trace("mac-policy", 0, 0);
    for (uint32_t i = 0; i < 4u; i++) {
        mac_rx_policy_word(i);
        mac_rx_set_policy(i);
    }
    osi_trace("mac-rxbuf", 0, 0);
    mac_rx_base_init();
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
