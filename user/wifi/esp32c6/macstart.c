/*
 * The register sequences of the libraries' bring-up, written out, so that the driver's own start makes them:
 * their hal_init's words about HAL_CFG, HAL_HOLD and HAL_MISC, its txrx queues, its receive policy and the words
 * around them, its receive base and its low-rate group, their hal_crypto_init's cipher words, and their coex PTI.
 * main.c calls these where the libraries' hal_init called the groups; the accesses each makes are the libraries',
 * and the trace holds them. They reach the device through macregs.h alone, so they build on the host for their replay;
 * the libraries' calls they leave are declared below.
 */

#include <stdint.h>

#include "mac.h"
#include "macregs.h"

/*
 * The libraries' hal_init's own register writes, around the groups of its MAC configuration that it calls:
 * their HAL_CFG start and the wait for it, and HAL_HOLD and HAL_MISC cleared.
 * Then, after the groups, HAL_HOLD, HAL_WORD and RX_WORD as their hal_init leaves them.
 * The driver's own start makes these, so that the groups it still calls sit where the libraries put them; see main.c.
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
