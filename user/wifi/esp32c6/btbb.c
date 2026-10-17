/*
 * The ESP32-C6's BT baseband, the driver's own: Espressif's libbtbb.a was taken into it whole, one function at a time,
 * as the Wi-Fi libraries were, see user/wifi/esp32c6/NOTES.md; every step is in git history, and the archive is linked
 * no more.
 * The archive transcribed is esp32-wifi-lib's, its bt_bb_v2.o reporting "libbtbb version: e4897a5, Jul 14 2026, 10:37:04".
 * Every function below is written from the disassembly of that archive's bt_bb_v2.o, and the 802.15.4 trace window and a
 * host replay hold it, against test/replay/bb.txt and bb-other.txt.
 * The baseband is shared with Wi-Fi's radio, but the only bring-up of it here is IEEE 802.15.4's, i154.c's.
 * Its words are reached through devregs.h's rd and wr, so that the bring-up's replay can answer them on the host;
 * what each one is is not known, so one that only these functions reach carries the offset it was reached at.
 */

#include <stdint.h>

#include "btbb.h"
#include "devices.h"
#include "devregs.h"

/*
 * BB_08 and BB_0C are written by bt_bb_v2_tx_set, BB_18 by bt_bb_v2_tx_set too, BB_10's high byte by
 * bt_bb_set_zb_tx_on_delay and its bits 5 to 12 by bt_bb_set_le_tx_on_delay, BB_B8 by bt_bb_tx_cca_period,
 * and BB_E0 to BB_F4 are the Gaussian filter's coefficients, written by set_gauss_coeff_1m and _2m.
 * The fields the archive's ble_tx_config_check names, from its own strings, see NOTES.md's table:
 * BB_10's bits 5 to 12 are bb_tx_on_delay, BB_18's bits 3 to 7 tx_ramp_delay and 19 to 23 tx_rampup_delay,
 * and BB_B8's bits 12 to 19 tx_cca_end_ts and 23 to 30 tx_cca_start_ts.
 */
#define BB_00 (BT_BB_BASE + 0x000u)
#define BB_08 (BT_BB_BASE + 0x008u)
#define BB_0C (BT_BB_BASE + 0x00cu)
#define BB_10 (BT_BB_BASE + 0x010u)
#define BB_18 (BT_BB_BASE + 0x018u)
#define BB_20 (BT_BB_BASE + 0x020u)
#define BB_24 (BT_BB_BASE + 0x024u)
#define BB_28 (BT_BB_BASE + 0x028u)
#define BB_2C (BT_BB_BASE + 0x02cu)
#define BB_30 (BT_BB_BASE + 0x030u)
#define BB_34 (BT_BB_BASE + 0x034u)
#define BB_38 (BT_BB_BASE + 0x038u)
#define BB_4C (BT_BB_BASE + 0x04cu)
#define BB_50 (BT_BB_BASE + 0x050u)
#define BB_54 (BT_BB_BASE + 0x054u)
#define BB_58 (BT_BB_BASE + 0x058u)
#define BB_60 (BT_BB_BASE + 0x060u)
#define BB_64 (BT_BB_BASE + 0x064u)
#define BB_68 (BT_BB_BASE + 0x068u)
#define BB_B8 (BT_BB_BASE + 0x0b8u)
#define BB_BC (BT_BB_BASE + 0x0bcu)
#define BB_C0 (BT_BB_BASE + 0x0c0u)
#define BB_FC (BT_BB_BASE + 0x0fcu)
#define BB_F8 (BT_BB_BASE + 0x0f8u)
#define BB_100 (BT_BB_BASE + 0x100u)
/* The word 0x600a4c24, in the HAL's block, which bt_bb_rx_set clears a bit of; what it is is not known. */
#define MODEM_4C24 (MODEM_BASE + 0x4c24u)
#define BB_E0 (BT_BB_BASE + 0x0e0u)
#define BB_E4 (BT_BB_BASE + 0x0e4u)
#define BB_E8 (BT_BB_BASE + 0x0e8u)
#define BB_EC (BT_BB_BASE + 0x0ecu)
#define BB_F0 (BT_BB_BASE + 0x0f0u)
#define BB_F4 (BT_BB_BASE + 0x0f4u)
/* The baseband's word 0x14, which btbb_init writes; what it is is not known. */
#define BB_INIT_14 (BT_BB_BASE + 0x014u)
/*
 * The word before the baseband's block, 0x600a08fc, whose bits 0 to 7 are rf_on_bt_delay and 8 to 15 pa_on_bt_delay,
 * from ble_tx_config_check's strings, see NOTES.md's table; btbb_init writes 0xc800, the pa_on_bt_delay 0xc8.
 */
#define BB_INIT_18FC 0x600a08fcu

/*
 * The 802.15.4 MAC's delays, ESP-IDF's IEEE802154_TXON_DELAY_REG and its neighbours TXOFF, RXON and TXRX_SWITCH,
 * see NOTES.md's table; ieee802154_txon_delay_set writes the four below.
 */
#define I154_TXON_DELAY_REG (I154_BASE + 0x104u)
#define I154_TXOFF_DELAY_REG (I154_BASE + 0x10cu)
#define I154_RXON_DELAY_REG (I154_BASE + 0x110u)
#define I154_TXRX_SWITCH_DELAY_REG (I154_BASE + 0x114u)

/*
 * The AGC's words, which bt_agc_recorrect_set, bt_agc_rssi_thresh and bt_agc_gain_offset write, at their modem offsets;
 * what each field is is not known.
 */
#define AGC_2840 (MODEM_BASE + 0x2840u)
#define AGC_2800 (MODEM_BASE + 0x2800u)
#define AGC_2824 (MODEM_BASE + 0x2824u)
#define AGC_2828 (MODEM_BASE + 0x2828u)
#define AGC_282C (MODEM_BASE + 0x282cu)
#define AGC_2844 (MODEM_BASE + 0x2844u)
#define AGC_284C (MODEM_BASE + 0x284cu)
#define AGC_2848 (MODEM_BASE + 0x2848u)
#define AGC_2850 (MODEM_BASE + 0x2850u)
#define AGC_2854 (MODEM_BASE + 0x2854u)
#define AGC_2894 (MODEM_BASE + 0x2894u)
#define AGC_2898 (MODEM_BASE + 0x2898u)
#define AGC_289C (MODEM_BASE + 0x289cu)
#define AGC_2888 (MODEM_BASE + 0x2888u)
#define AGC_288C (MODEM_BASE + 0x288cu)
#define AGC_2884 (MODEM_BASE + 0x2884u)
#define AGC_28DC (MODEM_BASE + 0x28dcu)
#define AGC_28E0 (MODEM_BASE + 0x28e0u)
#define AGC_28E4 (MODEM_BASE + 0x28e4u)
#define AGC_28E8 (MODEM_BASE + 0x28e8u)
#define AGC_28EC (MODEM_BASE + 0x28ecu)
#define AGC_28F0 (MODEM_BASE + 0x28f0u)
#define AGC_28C8 (MODEM_BASE + 0x28c8u)
#define AGC_28D0 (MODEM_BASE + 0x28d0u)
#define AGC_28D4 (MODEM_BASE + 0x28d4u)
#define AGC_28F4 (MODEM_BASE + 0x28f4u)
#define AGC_28A8 (MODEM_BASE + 0x28a8u)
#define AGC_28C0 (MODEM_BASE + 0x28c0u)
#define AGC_28C4 (MODEM_BASE + 0x28c4u)
#define AGC_2868 (MODEM_BASE + 0x2868u)
#define AGC_286C (MODEM_BASE + 0x286cu)
#define AGC_28A0 (MODEM_BASE + 0x28a0u)
#define AGC_28AC (MODEM_BASE + 0x28acu)
#define AGC_28B0 (MODEM_BASE + 0x28b0u)
#define AGC_28B4 (MODEM_BASE + 0x28b4u)
#define AGC_28B8 (MODEM_BASE + 0x28b8u)
#define AGC_28BC (MODEM_BASE + 0x28bcu)
/* The ZB receive's words, which zb_rx_auto_sel, zb_rx_core_set and zb_rx_v0_0_set write, at their modem offsets. */
#define ZB_2C00 (MODEM_BASE + 0x2c00u)
#define ZB_2C04 (MODEM_BASE + 0x2c04u)
#define ZB_2C08 (MODEM_BASE + 0x2c08u)
#define ZB_2C0C (MODEM_BASE + 0x2c0cu)
#define ZB_2C10 (MODEM_BASE + 0x2c10u)
#define ZB_2C14 (MODEM_BASE + 0x2c14u)
#define ZB_2C18 (MODEM_BASE + 0x2c18u)
#define ZB_2C20 (MODEM_BASE + 0x2c20u)
#define ZB_2C24 (MODEM_BASE + 0x2c24u)
#define ZB_2C2C (MODEM_BASE + 0x2c2cu)
#define ZB_2C30 (MODEM_BASE + 0x2c30u)
#define ZB_2C34 (MODEM_BASE + 0x2c34u)
#define ZB_2C48 (MODEM_BASE + 0x2c48u)
/* The Wi-Fi baseband's word 0xcd0, which bt_agc_gain_set sets a bit field of; what it is is not known. */
#define WIFI_BB_CD0 (WIFI_BB0_BASE + 0x0cd0u)

/*
 * The archive's byte the LE transmit-on delay is scaled from. bt_bb_tx_cca_en alone set it, and the bring-up never calls
 * that, so it is a constant zero here, as every traced run found it.
 */
static const uint8_t bt_bb_cca_en = 0;
/* libphy's calibration data, of which bt_agc_gain_set reads one byte; libphy's still, as the PHY is. */
extern const uint8_t phy_param[];
/* The ROM's, which fills the power table entry by entry; its three arguments' roles are not known. */
extern int get_data_sat(int, int, int);

/* The baseband's functions, declared where they are defined below, for the prototype. */
void bt_bb_v2_tx_set(uint32_t mode);
void set_gauss_coeff_1m(void);
void set_gauss_coeff_2m(void);
void bt_bb_set_le_tx_on_delay(uint32_t delay);
void bt_bb_set_zb_tx_on_delay(uint32_t delay);
void bt_bb_tx_cca_period(uint32_t first, uint32_t second);
void bt_bb_v2_rx_set(uint32_t value);
void bt_agc_recorrect_set(void);
void bt_agc_rssi_thresh(void);
void bt_agc_gain_offset(uint32_t value);
void bt_bb_rx_filter_sel(void);
void zb_rx_auto_sel(void);
void zb_rx_core_set(void);
void bt_set_rx_comp(uint32_t first, uint32_t second, uint32_t third, uint32_t fourth, uint32_t fifth);
void bt_agc_gain_max(uint32_t value);
void bt_agc_gain_set(void);
void zb_rx_v0_0_set(void);
void bt_agc_target_set(void);
void bt_agc_detect_set(void);
void bt_bb_rx_set(uint32_t value);
void bt_agc_v2_set(void);
void bt_bb_rx_correlator_set(void);
void bt_bb_rx_dpo_set(void);
void bt_agc_restart_set(void);

/*
 * The libraries' bt_bb_v2_init_cmplx, written out: the mode's transmit and receive set, the Gaussian filter's two
 * coefficient groups, three of their register writes, the transmit-on delays and the CCA period, the 802.15.4 MAC's
 * transmit-on delay, and the BER word's bit 7 cleared.
 * It is the driver's own now; their last call, bt_bb_v2_version, a print alone, is left out here, the driver's own log
 * serving where that line went.
 * The archive's own bt_bb_v2_init_cmplx, whose place this takes, is linked no more.
 * Their body asked the adapter's env_is_chip for one field, which the board answers true, so the chip's path is this.
 */
void btbb_init(void)
{
    bt_bb_v2_tx_set(0);
    bt_bb_v2_rx_set(4);
    set_gauss_coeff_1m();
    set_gauss_coeff_2m();
    wr(BB_INIT_18FC, (rd(BB_INIT_18FC) & 0xffff00ffu) | 0xc800u);
    wr(BB_INIT_14, (rd(BB_INIT_14) & 0xff00ffffu) | 0xc0000u);
    bt_bb_set_le_tx_on_delay((uint32_t)bt_bb_cca_en * 0x14u + 0x1du);
    bt_bb_tx_cca_period(0x30u, 0x14u);
    bt_bb_set_zb_tx_on_delay(0x1du);
    ieee802154_txon_delay_set();
    wr(BB_0C, rd(BB_0C) & ~0x80u);
}

/*
 * The libraries' bt_bb_v2_tx_set, written out: BB_18's bits 8 to 15 given 0x28 and its bits 0 to 7 given 0x0a,
 * BB_08's bits 17 to 22 given the argument's low six, and BB_0C's bit 8 set, each beside the bits kept.
 * Their body's argument is a mode the driver calls with zero.
 */
void bt_bb_v2_tx_set(uint32_t mode)
{
    wr(BB_18, (rd(BB_18) & 0xffff00ffu) | 0x2800u);
    wr(BB_18, (rd(BB_18) & 0xffffff00u) | 0x0au);
    wr(BB_08, (rd(BB_08) & 0xff81ffffu) | ((mode << 17) & 0x7e0000u));
    wr(BB_0C, (rd(BB_0C) & 0xfffff0ffu) | 0x100u);
}

/*
 * The libraries' set_gauss_coeff_1m, written out: the Gaussian filter's coefficients for the one-megabit mode,
 * six read-modify-writes on BB_E0 and three each on BB_E4, BB_E8 and BB_EC, every value their body's constant.
 */
void set_gauss_coeff_1m(void)
{
    wr(BB_E0, rd(BB_E0) & 0x0fffffffu);
    wr(BB_E0, rd(BB_E0) & 0xf07fffffu);
    wr(BB_E0, (rd(BB_E0) & 0xff81ffffu) | 0x40000u);
    wr(BB_E0, (rd(BB_E0) & 0xfffe03ffu) | 0x2800u);
    wr(BB_E0, (rd(BB_E0) & 0xfffffc03u) | 0xb8u);
    wr(BB_E4, (rd(BB_E4) & 0x7fffffu) | 0x4d800000u);
    wr(BB_E4, (rd(BB_E4) & 0xff801fffu) | 0x31e000u);
    wr(BB_E4, (rd(BB_E4) & 0xffffe003u) | 0xc60u);
    wr(BB_E8, (rd(BB_E8) & 0x1fffffu) | 0x9cc00000u);
    wr(BB_E8, (rd(BB_E8) & 0xffe003ffu) | 0x19c000u);
    wr(BB_EC, (rd(BB_EC) & 0x1fffffu) | 0xec400000u);
    wr(BB_EC, (rd(BB_EC) & 0xffe003ffu) | 0x1f1c00u);
}

/*
 * The libraries' set_gauss_coeff_2m, written out: the same filter's coefficients for the two-megabit mode,
 * three read-modify-writes on BB_F0 and three on BB_F4, every value their body's constant.
 */
void set_gauss_coeff_2m(void)
{
    wr(BB_F0, rd(BB_F0) & 0x1ffffffu);
    wr(BB_F0, (rd(BB_F0) & 0xfe01ffffu) | 0xe0000u);
    wr(BB_F0, (rd(BB_F0) & 0xfffe00ffu) | 0x6900u);
    wr(BB_F4, (rd(BB_F4) & 0x3fffffu) | 0x96000000u);
    wr(BB_F4, (rd(BB_F4) & 0xffc007ffu) | 0x2d3800u);
    wr(BB_F4, (rd(BB_F4) & 0xfffff800u) | 0x78fu);
}

/*
 * The libraries' bt_bb_set_le_tx_on_delay, written out: BB_10's bits 2 to 12 given ((delay << 5) - 0xa0) masked to
 * bits 5 to 12, the other bits kept. Their name says the transmit-on delay of the LE mode; what the field is is not known.
 */
void bt_bb_set_le_tx_on_delay(uint32_t delay)
{
    wr(BB_10, (rd(BB_10) & 0xffffe003u) | (((delay << 5) - 0xa0u) & 0x1fe0u));
}

/*
 * The libraries' bt_bb_set_zb_tx_on_delay, written out: BB_10's bits 24 to 31 given delay less five, its bits 0 to 20
 * kept. Their name says the transmit-on delay of the ZB (IEEE 802.15.4) mode; the same word as the LE delay's,
 * whose bits 21 to 23 this clears.
 */
void bt_bb_set_zb_tx_on_delay(uint32_t delay)
{
    wr(BB_10, (rd(BB_10) & 0x001fffffu) | (((delay - 5u) & 0xffu) << 24));
}

/*
 * The libraries' bt_bb_tx_cca_period, written out: BB_B8's bits 23 to 30 given first less second less five,
 * then its bits 9 to 19 given ((first - 5) << 12) - 0x200, the other bits kept.
 * Their body reads the word back between the two writes, as written below.
 * Their name says the transmit CCA period; the two arguments' roles are not known.
 */
void bt_bb_tx_cca_period(uint32_t first, uint32_t second)
{
    wr(BB_B8, (rd(BB_B8) & 0x800fffffu) | (((first - second - 5u) << 23) & 0x7f800000u));
    wr(BB_B8, (rd(BB_B8) & 0xfff001ffu) | ((((first - 5u) << 12) - 0x200u) & 0xffe00u));
}

/*
 * The libraries' ieee802154_txon_delay_set, written out: four read-modify-writes on the 802.15.4 MAC's delays,
 * TXON_DELAY and TXRX_SWITCH_DELAY given 0x23 and 0x7f in their low ten bits and RXON_DELAY 0x32 in its low eleven,
 * then TXOFF_DELAY's low six cleared, their names ESP-IDF's, see NOTES.md's table.
 */
void ieee802154_txon_delay_set(void)
{
    wr(I154_TXON_DELAY_REG, (rd(I154_TXON_DELAY_REG) & 0xfffffc00u) | 0x23u);
    wr(I154_RXON_DELAY_REG, (rd(I154_RXON_DELAY_REG) & 0xfffff800u) | 0x32u);
    wr(I154_TXRX_SWITCH_DELAY_REG, (rd(I154_TXRX_SWITCH_DELAY_REG) & 0xfffffc00u) | 0x7fu);
    wr(I154_TXOFF_DELAY_REG, rd(I154_TXOFF_DELAY_REG) & 0xffffffc0u);
}

/*
 * The table bt_bb_get_tx_pwr_table fills and returns, 16 entries.
 */
static int8_t bt_bb_pwr_table[16];

/*
 * The libraries' bt_bb_get_tx_pwr_table, written out: 16 into the length's byte, then 16 times the ROM's get_data_sat
 * with the running index -0x18 to 0x18 by three, 0x14 and -0xf, its byte stored entry by entry, and the table returned.
 * It reaches no device, so the trace and the host replay hold none of it.
 */
const int8_t *bt_bb_get_tx_pwr_table(uint8_t *length)
{
    int8_t *p = bt_bb_pwr_table;
    *length = 0x10u;
    for (int i = -0x18; i != 0x18; i += 3) {
        *p++ = (int8_t)get_data_sat(i, 0x14, -0xf);
    }
    return bt_bb_pwr_table;
}

/*
 * The libraries' bt_bb_v2_rx_set, written out: the receive set with the bring-up's argument, then the receive
 * compensation's five numbers, the AGC gain offset 0x50, and then the AGC's, the correlator's, the DPO's and the
 * filter's groups and zb_rx_core_set, zb_rx_v0_0_set and zb_rx_auto_sel, each called with no argument.
 * Its own body writes no register, so the trace and the replay hold no access of its own; the functions it calls are the
 * driver's too now.
 */
void bt_bb_v2_rx_set(uint32_t value)
{
    bt_bb_rx_set(value);
    bt_set_rx_comp(0xc, -0x2, -0x6, -0x6, 0x0);
    bt_agc_gain_offset(0x50);
    bt_agc_gain_set();
    bt_agc_rssi_thresh();
    bt_agc_target_set();
    bt_agc_restart_set();
    bt_agc_recorrect_set();
    bt_agc_detect_set();
    bt_bb_rx_correlator_set();
    bt_bb_rx_dpo_set();
    bt_bb_rx_filter_sel();
    zb_rx_core_set();
    zb_rx_v0_0_set();
    zb_rx_auto_sel();
}

/*
 * The libraries' bt_agc_recorrect_set, written out: twelve read-modify-writes on the AGC's five words, each a constant
 * beside the bits kept, the last 0x600a286c's top byte given 0x0f. What the fields are is not known.
 */
void bt_agc_recorrect_set(void)
{
    wr(AGC_28B4, rd(AGC_28B4) | 0x100u);
    wr(AGC_28B4, (rd(AGC_28B4) & 0xffffc1ffu) | 0x1400u);
    wr(AGC_28BC, rd(AGC_28BC) | 0xa0u);
    wr(AGC_2868, rd(AGC_2868) | 0x2000u);
    wr(AGC_28BC, rd(AGC_28BC) & ~0x140u);
    wr(AGC_2868, rd(AGC_2868) | 0x20u);
    wr(AGC_2868, rd(AGC_2868) | 0x1800u);
    wr(AGC_2868, (rd(AGC_2868) & ~0x7c0u) | 0x280u);
    wr(AGC_28B0, (rd(AGC_28B0) & 0xffffc1ffu) | 0x1400u);
    wr(AGC_28B0, (rd(AGC_28B0) & ~0x1f0u) | 0xa0u);
    wr(AGC_2868, (rd(AGC_2868) & ~0x1fu) | 0xau);
    wr(AGC_286C, (rd(AGC_286C) & 0x00ffffffu) | 0x0f000000u);
}

/*
 * The libraries' bt_agc_rssi_thresh, written out: three read-modify-writes on the AGC's words 0x28a0, 0x28ac and
 * 0x28b8, each its constant beside the bits kept. Their name says an RSSI threshold; the fields are not known.
 */
void bt_agc_rssi_thresh(void)
{
    wr(AGC_28A0, (rd(AGC_28A0) & 0xff00ffffu) | 0x009c0000u);
    wr(AGC_28AC, (rd(AGC_28AC) & 0x807fffffu) | 0x44000000u);
    wr(AGC_28B8, (rd(AGC_28B8) & 0xfff00fffu) | 0x00088000u);
}

/*
 * The libraries' bt_agc_gain_offset, written out: on 0x2848 and then on 0x2868, the argument's low 16 bits into the
 * word's bits 16 to 31 and its low 8 into bits 24 to 31, the bits below kept, each word read again between the two.
 */
void bt_agc_gain_offset(uint32_t value)
{
    wr(AGC_2848, (rd(AGC_2848) & 0xff00ffffu) | (value << 16));
    wr(AGC_2848, (rd(AGC_2848) & 0x00ffffffu) | (value << 24));
    wr(AGC_2868, (rd(AGC_2868) & 0x00ffffffu) | (value << 24));
    wr(AGC_2868, (rd(AGC_2868) & 0xff00ffffu) | (value << 16));
}

/*
 * The libraries' bt_bb_rx_filter_sel, written out: five read-modify-writes on the baseband's 0xfc, each a mask
 * beside the bits kept, two of them a constant too. Their name says which receive filter is chosen; the fields are
 * not known.
 */
void bt_bb_rx_filter_sel(void)
{
    wr(BB_FC, rd(BB_FC) & 0xfe7fffffu);
    wr(BB_FC, (rd(BB_FC) & 0xff9fffffu) | 0x00200000u);
    wr(BB_FC, rd(BB_FC) & 0xffe7ffffu);
    wr(BB_FC, rd(BB_FC) & 0xfff9ffffu);
    wr(BB_FC, (rd(BB_FC) & 0xfffe7fffu) | 0x00010000u);
}

/*
 * The libraries' zb_rx_auto_sel, written out: 0x2c48's bit 31 cleared, then the bits under 0xc03fffff kept and
 * 0x28400000 or'd. What the fields are is not known.
 */
void zb_rx_auto_sel(void)
{
    wr(ZB_2C48, rd(ZB_2C48) & ~0x80000000u);
    wr(ZB_2C48, (rd(ZB_2C48) & 0xc03fffffu) | 0x28400000u);
}

/*
 * The libraries' zb_rx_core_set, written out: six read-modify-writes on the ZB receive's words 0x2c04, 0x2c08, 0x2c20
 * and 0x2c24, each a constant beside the bits kept, two of them an 8-bit field kept first. What they are is not known.
 */
void zb_rx_core_set(void)
{
    wr(ZB_2C04, (rd(ZB_2C04) & 0xffffu) | 0x0bb80000u);
    wr(ZB_2C08, (rd(ZB_2C08) & 0xffffu) | 0x17700000u);
    wr(ZB_2C08, (rd(ZB_2C08) & 0xffff0000u) | 0xbb8u);
    wr(ZB_2C04, (rd(ZB_2C04) & ~0x3fcu) | 0x284u);
    wr(ZB_2C20, (rd(ZB_2C20) & 0xffu) | 0x07a12000u);
    wr(ZB_2C24, (rd(ZB_2C24) & 0xffu) | 0xf85edf00u);
}

/*
 * The libraries' bt_set_rx_comp, written out: five read-modify-writes on the AGC's word 0x2844 -- the first argument's
 * low 7 bits into its bits 7 to 13 and then into its low 7, the fourth's low 5 into bits 24 to 28, the third's low 5
 * into bits 19 to 23, and the second's low 5 into bits 14 to 18. Their body takes a fifth and does not use it.
 */
void bt_set_rx_comp(uint32_t first, uint32_t second, uint32_t third, uint32_t fourth, uint32_t fifth)
{
    (void)fifth;
    wr(AGC_2844, (rd(AGC_2844) & 0xffffc07fu) | ((first & 0x7fu) << 7));
    wr(AGC_2844, (rd(AGC_2844) & ~0x7fu) | (first & 0x7fu));
    wr(AGC_2844, (rd(AGC_2844) & 0xe0ffffffu) | ((fourth << 24) & 0x1f000000u));
    wr(AGC_2844, (rd(AGC_2844) & 0xff07ffffu) | ((third << 19) & 0x00f80000u));
    wr(AGC_2844, (rd(AGC_2844) & 0xfff83fffu) | ((second << 14) & 0x0007c000u));
}

/*
 * The libraries' bt_agc_gain_max, written out: the argument's low 7 bits into 0x2850's bits 14 to 20, beside the bits
 * kept, and the same low 7 into 0x2840's bits 0 to 6 after those are cleared. What they are is not known.
 */
void bt_agc_gain_max(uint32_t value)
{
    wr(AGC_2850, (rd(AGC_2850) & 0xffe03fffu) | ((value << 14) & 0x001fc000u));
    wr(AGC_2840, (rd(AGC_2840) & ~0x7fu) | (value & 0x7fu));
}

/*
 * The libraries' bt_agc_gain_set, written out: the Wi-Fi baseband's 0x7cd0 given 0xf000b, then libphy's phy_param's
 * byte 0x120 handed to bt_agc_gain_max, then three read-modify-writes on the AGC's 0x2854 -- bit 0 set, bits 2 to 8
 * given 0xc8, and bit 0 set again.
 */
void bt_agc_gain_set(void)
{
    wr(WIFI_BB_CD0, rd(WIFI_BB_CD0) | 0xf000bu);
    bt_agc_gain_max(phy_param[0x120]);
    wr(AGC_2854, rd(AGC_2854) | 0x1u);
    wr(AGC_2854, (rd(AGC_2854) & ~0x1fcu) | 0xc8u);
    wr(AGC_2854, rd(AGC_2854) | 0x1u);
}

/*
 * The libraries' zb_rx_v0_0_set, written out: eleven read-modify-writes on the ZB receive's words 0x2c0c, 0x2c10,
 * 0x2c14, 0x2c18, 0x2c2c, 0x2c30 and 0x2c34, each a constant beside the bits kept, three of them an 8-bit field first.
 */
void zb_rx_v0_0_set(void)
{
    wr(ZB_2C10, (rd(ZB_2C10) & 0x1fffffu) | 0xf4200000u);
    wr(ZB_2C0C, (rd(ZB_2C0C) & 0xffe003ffu) | 0x001ebc00u);
    wr(ZB_2C18, (rd(ZB_2C18) & ~0x7ffu) | 0x7afu);
    wr(ZB_2C10, (rd(ZB_2C10) & 0xffe01fffu) | 0x0014c000u);
    wr(ZB_2C34, rd(ZB_2C34) & 0x7ffffeffu);
    wr(ZB_2C34, (rd(ZB_2C34) & 0x800fffffu) | 0x7e100000u);
    wr(ZB_2C34, (rd(ZB_2C34) & 0xfff001ffu) | 0x000fda00u);
    wr(ZB_2C2C, (rd(ZB_2C2C) & 0xffu) | 0x07a12000u);
    wr(ZB_2C30, (rd(ZB_2C30) & 0xffu) | 0xf85edf00u);
    wr(ZB_2C14, rd(ZB_2C14) & ~0x2u);
    wr(ZB_2C0C, rd(ZB_2C0C) | 0x200u);
}

/*
 * The libraries' bt_agc_target_set, written out: thirteen read-modify-writes on the AGC's words 0x2840, 0x2894, 0x2898,
 * 0x289c, 0x28b0 and 0x28b4, each a constant beside the bits kept. Their name says an AGC target; the fields are not
 * known.
 */
void bt_agc_target_set(void)
{
    wr(AGC_2898, (rd(AGC_2898) & ~0x1ffu) | 0x1ceu);
    wr(AGC_289C, (rd(AGC_289C) & ~0x1ffu) | 0x1ceu);
    wr(AGC_28B0, (rd(AGC_28B0) & 0x7fffffu) | 0xe7000000u);
    wr(AGC_28B0, (rd(AGC_28B0) & 0xff803fffu) | 0x738000u);
    wr(AGC_2840, (rd(AGC_2840) & 0xffc01fffu) | 0x39c000u);
    wr(AGC_289C, (rd(AGC_289C) & 0xfffc01ffu) | 0x39c00u);
    wr(AGC_2898, (rd(AGC_2898) & 0xffff81ffu) | 0x1800u);
    wr(AGC_289C, (rd(AGC_289C) & 0xff03ffffu) | 0x300000u);
    wr(AGC_2840, (rd(AGC_2840) & 0xffffe3ffu) | 0x800u);
    wr(AGC_2894, (rd(AGC_2894) & 0xff803fffu) | 0x738000u);
    wr(AGC_2894, (rd(AGC_2894) & 0x7fffffu) | 0xe7000000u);
    wr(AGC_28B4, (rd(AGC_28B4) & 0x7fffffu) | 0xe7000000u);
    wr(AGC_28B4, (rd(AGC_28B4) & 0xff803fffu) | 0x738000u);
}

/*
 * The libraries' bt_agc_detect_set, written out: fourteen read-modify-writes on the AGC's words 0x28a0, 0x28a8,
 * 0x28ac, 0x28b8, 0x28c0 and 0x28c4, each a constant beside the bits kept. Their name says AGC detection; the fields
 * are not known.
 */
void bt_agc_detect_set(void)
{
    wr(AGC_28A0, (rd(AGC_28A0) & 0xf0ffffffu) | 0x04000000u);
    wr(AGC_28C0, (rd(AGC_28C0) & ~0x3e0u) | 0x140u);
    wr(AGC_28A8, (rd(AGC_28A8) & 0xff83ffffu) | 0x00100000u);
    wr(AGC_28A0, (rd(AGC_28A0) & 0xcfffffffu) | 0x20000000u);
    wr(AGC_28AC, (rd(AGC_28AC) & 0xff87ffffu) | 0x00380000u);
    wr(AGC_28C0, (rd(AGC_28C0) & 0xffff83ffu) | 0x2800u);
    wr(AGC_28B8, (rd(AGC_28B8) & 0xfffff0ffu) | 0x700u);
    wr(AGC_28C4, (rd(AGC_28C4) & 0xffff83ffu) | 0x3c00u);
    wr(AGC_28C0, (rd(AGC_28C0) & 0x00ffffffu) | 0x9c000000u);
    wr(AGC_28C0, (rd(AGC_28C0) & 0xff0fffffu) | 0x700000u);
    wr(AGC_28C0, (rd(AGC_28C0) & 0xfff07fffu) | 0x50000u);
    wr(AGC_28C4, (rd(AGC_28C4) & 0x00ffffffu) | 0x9c000000u);
    wr(AGC_28C4, (rd(AGC_28C4) & 0xff0fffffu) | 0xa00000u);
    wr(AGC_28C4, (rd(AGC_28C4) & 0xfff07fffu) | 0x78000u);
}

/*
 * The libraries' bt_agc_v2_set, written out: the baseband's 0x58 bits 3 and 2 set, the AGC's 0x2854 bit 0 set, then
 * sixteen read-modify-writes on the AGC's 0x2824, 0x2828, 0x282c, 0x28a0, 0x28c8, 0x28d0 and 0x28d4, each a constant
 * beside the bits kept. What they are is not known.
 */
void bt_agc_v2_set(void)
{
    wr(BB_58, rd(BB_58) | 0x8u);
    wr(BB_58, rd(BB_58) | 0x4u);
    wr(AGC_2854, rd(AGC_2854) | 0x1u);
    wr(AGC_282C, (rd(AGC_282C) & 0xfffff01fu) | 0x140u);
    wr(AGC_2828, (rd(AGC_2828) & 0xffe03fffu) | 0x2c000u);
    wr(AGC_2828, (rd(AGC_2828) & ~0x7fu) | 0x2du);
    wr(AGC_2828, (rd(AGC_2828) & 0xf01fffffu) | 0x5a00000u);
    wr(AGC_28C8, (rd(AGC_28C8) & 0x803fffffu) | 0x7d800000u);
    wr(AGC_28C8, (rd(AGC_28C8) & 0xffc01fffu) | 0x34c000u);
    wr(AGC_28D0, (rd(AGC_28D0) & 0x1ffffffu) | 0x3c000000u);
    wr(AGC_28D0, (rd(AGC_28D0) & 0xfe03ffffu) | 0x500000u);
    wr(AGC_28D0, (rd(AGC_28D0) & 0xfffc07ffu) | 0x5000u);
    wr(AGC_28D0, (rd(AGC_28D0) & ~0x7f0u) | 0xa0u);
    wr(AGC_28D4, (rd(AGC_28D4) & 0x1ffffffu) | 0x14000000u);
    wr(AGC_28A0, rd(AGC_28A0) & ~0xffu);
    wr(AGC_28A0, (rd(AGC_28A0) & 0xffff00ffu) | 0x5a00u);
    wr(AGC_2824, (rd(AGC_2824) & 0xf01fffffu) | 0x1400000u);
}

/*
 * The libraries' bt_bb_rx_set, written out: the baseband's 0x50's low eleven bits cleared, 0x8's bits 23 to 28 given
 * the argument's low six, 0xc's low twelve given 0x199a000, the HAL's 0x4c24 bit 31 cleared, 0x4c bit 2 set,
 * then bt_agc_v2_set, and then the rest: the AGC's 0x2888 bit 29 cleared and bit 18 set, the baseband's 0xbc bit 31
 * set, 0xc0's low two cleared, 0x54's bit 5 cleared and bits 2 and 4 set, 0x00 bit 0 set, the AGC's 0x2800 bits 17 to
 * 19 and 10 to 12 set, 0x284c bit 26 cleared and 0x288c bit 29 cleared, and the ZB receive's 0x2c00's low twelve given
 * 0x199a000, 0x2c04's bit 11 cleared and 0x2c0c's bit 8 cleared.
 */
void bt_bb_rx_set(uint32_t value)
{
    wr(BB_50, rd(BB_50) & ~0x7ffu);
    wr(BB_08, (rd(BB_08) & 0xe07fffffu) | ((value << 23) & 0x1f800000u));
    wr(BB_0C, (rd(BB_0C) & 0xfffu) | 0x199a000u);
    wr(MODEM_4C24, rd(MODEM_4C24) & 0x7fffffffu);
    wr(BB_4C, rd(BB_4C) | 0x4u);
    bt_agc_v2_set();
    wr(AGC_2888, rd(AGC_2888) & 0xdfffffffu);
    wr(AGC_2888, rd(AGC_2888) | 0x40000u);
    wr(BB_BC, rd(BB_BC) | 0x80000000u);
    wr(BB_C0, rd(BB_C0) & ~0x2u);
    wr(AGC_2800, rd(AGC_2800) | 0xe0000u);
    wr(AGC_2800, rd(AGC_2800) | 0x1c00u);
    wr(AGC_284C, rd(AGC_284C) & 0xfbffffffu);
    wr(AGC_288C, rd(AGC_288C) & 0xdfffffffu);
    wr(BB_54, (rd(BB_54) & ~0x1fu) | 0x14u);
    wr(BB_00, rd(BB_00) | 0x1u);
    wr(ZB_2C0C, rd(ZB_2C0C) & ~0x100u);
    wr(ZB_2C00, (rd(ZB_2C00) & 0xfffu) | 0x199a000u);
    wr(ZB_2C04, rd(ZB_2C04) & 0xfffff7ffu);
}

/*
 * The libraries' bt_bb_rx_correlator_set, written out: eighteen read-modify-writes on the baseband's 0x60, 0x64, 0x68,
 * 0xf8, 0xfc and 0x100, each a constant beside the bits kept, four of them a negative-andi mask. What they are is not
 * known.
 */
void bt_bb_rx_correlator_set(void)
{
    wr(BB_64, (rd(BB_64) & 0xfc0fffffu) | 0x300000u);
    wr(BB_64, (rd(BB_64) & 0xffffc0ffu) | 0x900u);
    wr(BB_FC, (rd(BB_FC) & 0x81ffffffu) | 0x12000000u);
    wr(BB_64, (rd(BB_64) & ~0xfcu) | 0x8u);
    wr(BB_68, (rd(BB_68) & 0x3ffffffu) | 0x10000000u);
    wr(BB_68, (rd(BB_68) & ~0x7e0u) | 0x60u);
    wr(BB_68, (rd(BB_68) & 0xfc0fffffu) | 0xd00000u);
    wr(BB_68, (rd(BB_68) & 0xfff00fffu) | 0x9b000u);
    wr(BB_68, rd(BB_68) | 0x800u);
    wr(BB_60, (rd(BB_60) & 0x3ffffffu) | 0x10000000u);
    wr(BB_100, (rd(BB_100) & 0xff3fffffu) | 0x400000u);
    wr(BB_100, rd(BB_100) & 0xf3ffffffu);
    wr(BB_64, (rd(BB_64) & 0xfff03fffu) | 0x10000u);
    wr(BB_F8, (rd(BB_F8) & 0xfe07ffffu) | 0x100000u);
    wr(BB_F8, (rd(BB_F8) & ~0x7e0u) | 0x60u);
    wr(BB_F8, (rd(BB_F8) & 0xfff807ffu) | 0x4f000u);
    wr(BB_F8, (rd(BB_F8) & 0x81ffffffu) | 0x2000000u);
    wr(BB_68, rd(BB_68) | 0x10u);
}

/*
 * The libraries' bt_bb_rx_dpo_set, written out: twenty read-modify-writes on the baseband's 0x20, 0x24, 0x28, 0x2c, 0x30,
 * 0x34 and 0x38 and on the AGC's 0x28f4, each a constant beside the bits kept, five of them a negative-andi mask.
 */
void bt_bb_rx_dpo_set(void)
{
    wr(BB_38, (rd(BB_38) & ~0x780u) | 0x300u);
    wr(BB_24, (rd(BB_24) & 0x87ffffffu) | 0x30000000u);
    wr(BB_34, (rd(BB_34) & 0x0fffffffu) | 0x70000000u);
    wr(BB_34, (rd(BB_34) & 0xf0ffffffu) | 0x7000000u);
    wr(BB_20, (rd(BB_20) & 0xfffc3fffu) | 0x18000u);
    wr(BB_20, (rd(BB_20) & 0xffffc3ffu) | 0x1c00u);
    wr(BB_34, (rd(BB_34) & 0xff0fffffu) | 0x700000u);
    wr(BB_34, (rd(BB_34) & 0xfff0ffffu) | 0x70000u);
    wr(BB_20, (rd(BB_20) & ~0x3fu) | 0x6u);
    wr(BB_24, rd(BB_24) & 0xfbffffffu);
    wr(BB_28, rd(BB_28) & ~0x30u);
    wr(BB_2C, (rd(BB_2C) & 0xfffff00fu) | 0xdc0u);
    wr(BB_28, rd(BB_28) & ~0xcu);
    wr(BB_2C, (rd(BB_2C) & ~0x6u) | 0x4u);
    wr(BB_30, (rd(BB_30) & ~0x3fu) | 0x3u);
    wr(AGC_28F4, (rd(AGC_28F4) & 0xfff00fffu) | 0xa000u);
    wr(AGC_28F4, (rd(AGC_28F4) & 0xe01fffffu) | 0x5000000u);
    wr(AGC_28F4, rd(AGC_28F4) & 0xbfffffffu);
    wr(AGC_28F4, rd(AGC_28F4) & 0x7fffffffu);
    wr(AGC_28F4, rd(AGC_28F4) & 0xdfffffffu);
}

/*
 * The libraries' bt_agc_restart_set, written out: twenty-seven read-modify-writes on the AGC's 0x2884, 0x2888, 0x28bc,
 * 0x28dc, 0x28e0, 0x28e4, 0x28e8, 0x28ec and 0x28f0, each a constant beside the bits kept, five of them a
 * negative-andi mask. Their name says an AGC restart; the fields are not known.
 */
void bt_agc_restart_set(void)
{
    wr(AGC_2888, rd(AGC_2888) & 0xefffffffu);
    wr(AGC_28BC, rd(AGC_28BC) | 0x80000000u);
    wr(AGC_2884, rd(AGC_2884) | 0x00800000u);
    wr(AGC_2884, (rd(AGC_2884) & 0x00ffffffu) | 0xf4000000u);
    wr(AGC_28E0, (rd(AGC_28E0) & 0x00ffffffu) | 0x14000000u);
    wr(AGC_28E0, (rd(AGC_28E0) & 0xff00ffffu) | 0x00140000u);
    wr(AGC_28E0, (rd(AGC_28E0) & 0xffff00ffu) | 0x1400u);
    wr(AGC_28E4, (rd(AGC_28E4) & 0xff00ffffu) | 0xec0000u);
    wr(AGC_28E4, (rd(AGC_28E4) & 0xffff00ffu) | 0xec00u);
    wr(AGC_28E4, (rd(AGC_28E4) & ~0xffu) | 0xecu);
    wr(AGC_28E0, (rd(AGC_28E0) & ~0xffu) | 0x14u);
    wr(AGC_28E8, (rd(AGC_28E8) & 0x00ffffffu) | 0xec000000u);
    wr(AGC_28EC, (rd(AGC_28EC) & 0x00ffffffu) | 0x5a000000u);
    wr(AGC_28EC, (rd(AGC_28EC) & 0xffff00ffu) | 0xa600u);
    wr(AGC_28F0, (rd(AGC_28F0) & 0x00ffffffu) | 0xa0000000u);
    wr(AGC_28E4, (rd(AGC_28E4) & 0x00ffffffu) | 0x19000000u);
    wr(AGC_28E8, (rd(AGC_28E8) & 0xff00ffffu) | 0xe70000u);
    wr(AGC_28EC, (rd(AGC_28EC) & 0xff00ffffu) | 0x2d0000u);
    wr(AGC_28EC, (rd(AGC_28EC) & ~0xffu) | 0xd3u);
    wr(AGC_28F0, (rd(AGC_28F0) & 0xff00ffffu) | 0xa60000u);
    wr(AGC_28DC, (rd(AGC_28DC) & ~0x700u) | 0x600u);
    wr(AGC_28DC, (rd(AGC_28DC) & 0xffffc7ffu) | 0x3000u);
    wr(AGC_28DC, (rd(AGC_28DC) & ~0xe0u) | 0xc0u);
    wr(AGC_28DC, (rd(AGC_28DC) & ~0x1cu) | 0x18u);
    wr(AGC_28DC, (rd(AGC_28DC) & 0xfffe3fffu) | 0x18000u);
    wr(AGC_28E8, (rd(AGC_28E8) & 0xffff03ffu) | 0xc00u);
    wr(AGC_28DC, rd(AGC_28DC) | 0x10000000u);
}
