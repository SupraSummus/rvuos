/*
 * The ESP32-C6's BT baseband, the driver's own: Espressif's libbtbb.a is being taken from it one function at a time,
 * as the Wi-Fi libraries were, see user/wifi/esp32c6/NOTES.md.
 * Each function below is written from the disassembly of the archive's bt_bb_v2.o, and the 802.15.4 trace window and a
 * host replay hold it; what is not the driver's yet stays linked from libbtbb.a, and build.mk's WIFI_ESP_BTBB_OWN
 * weakens the taken two so that the library's own calls reach these.
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
 */
#define BB_08 (BT_BB_BASE + 0x008u)
#define BB_0C (BT_BB_BASE + 0x00cu)
#define BB_10 (BT_BB_BASE + 0x010u)
#define BB_18 (BT_BB_BASE + 0x018u)
#define BB_B8 (BT_BB_BASE + 0x0b8u)
#define BB_FC (BT_BB_BASE + 0x0fcu)
#define BB_E0 (BT_BB_BASE + 0x0e0u)
#define BB_E4 (BT_BB_BASE + 0x0e4u)
#define BB_E8 (BT_BB_BASE + 0x0e8u)
#define BB_EC (BT_BB_BASE + 0x0ecu)
#define BB_F0 (BT_BB_BASE + 0x0f0u)
#define BB_F4 (BT_BB_BASE + 0x0f4u)
/* The baseband's word 0x14, which btbb_init writes; what it is is not known. */
#define BB_INIT_14 (BT_BB_BASE + 0x014u)
/* The word before the baseband's block, 0x600a08fc, which btbb_init writes; what it is is not known. */
#define BB_INIT_18FC 0x600a08fcu

/* The 802.15.4 MAC's transmit-on delay words, which ieee802154_txon_delay_set writes; the block is devices.h's. */
#define I154_104 (I154_BASE + 0x104u)
#define I154_10C (I154_BASE + 0x10cu)
#define I154_110 (I154_BASE + 0x110u)
#define I154_114 (I154_BASE + 0x114u)

/*
 * The AGC's words, which bt_agc_recorrect_set, bt_agc_rssi_thresh and bt_agc_gain_offset write, at their modem offsets;
 * what each field is is not known.
 */
#define AGC_2840 (MODEM_BASE + 0x2840u)
#define AGC_2844 (MODEM_BASE + 0x2844u)
#define AGC_2848 (MODEM_BASE + 0x2848u)
#define AGC_2850 (MODEM_BASE + 0x2850u)
#define AGC_2854 (MODEM_BASE + 0x2854u)
#define AGC_2894 (MODEM_BASE + 0x2894u)
#define AGC_2898 (MODEM_BASE + 0x2898u)
#define AGC_289C (MODEM_BASE + 0x289cu)
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

/* The archive's functions the bring-up still calls, of libbtbb.a until the driver's own replace them. */
extern void bt_bb_v2_version(void);
/* The archive's functions bt_bb_v2_rx_set calls, theirs until the driver's own replace them. */
extern void bt_bb_rx_set(uint32_t value);
extern void bt_agc_gain_offset(uint32_t value);
extern void bt_agc_rssi_thresh(void);
extern void bt_agc_restart_set(void);
extern void bt_bb_rx_correlator_set(void);
extern void bt_bb_rx_dpo_set(void);
extern void bt_bb_rx_filter_sel(void);
extern void zb_rx_core_set(void);
extern void zb_rx_auto_sel(void);
/* The archive's byte the LE transmit-on delay is scaled from, theirs until its cca functions are the driver's too. */
extern uint8_t bt_bb_cca_en;
/* libphy's calibration data, of which bt_agc_gain_set reads one byte; theirs, as the PHY is libphy's. */
extern const uint8_t phy_param[];
/* The ROM's, which fills the power table entry by entry; its three arguments' roles are not known. */
extern int get_data_sat(int, int, int);

/* The archive's functions the driver's own take, declared where they are defined below, for the prototype. */
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

/*
 * The libraries' bt_bb_v2_init_cmplx, written out: the mode's transmit and receive set, the Gaussian filter's two
 * coefficient groups, three of their register writes, the transmit-on delays and the CCA period, the 802.15.4 MAC's
 * transmit-on delay, the BER word's bit 7 cleared, and their version printed when the argument asks.
 * It is the driver's own now, and the calls it makes go over to the driver's own bodies one at a time;
 * the archive's own bt_bb_v2_init_cmplx is left uncalled and uncalled-for once every one of them has.
 * Their body asked the adapter's env_is_chip for one field, which the board answers true, so the chip's path is this.
 */
void btbb_init(int print_version)
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
    if (print_version) {
        bt_bb_v2_version();
    }
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
 * The libraries' ieee802154_txon_delay_set, written out: four read-modify-writes on the 802.15.4 MAC's words 0x104,
 * 0x110 and 0x114, each given its constant in the low ten or eleven bits, then 0x10c's low six cleared.
 * Their name says the transmit-on delay of IEEE 802.15.4; the fields' meaning is not known.
 */
void ieee802154_txon_delay_set(void)
{
    wr(I154_104, (rd(I154_104) & 0xfffffc00u) | 0x23u);
    wr(I154_110, (rd(I154_110) & 0xfffff800u) | 0x32u);
    wr(I154_114, (rd(I154_114) & 0xfffffc00u) | 0x7fu);
    wr(I154_10C, rd(I154_10C) & 0xffffffc0u);
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
 * Its own body writes no register, so neither the trace nor the replay holds it, and it is build.mk's
 * WIFI_ESP_BTBB_BARE; its calls go over to the driver's own bodies one at a time, as btbb_init's do.
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
