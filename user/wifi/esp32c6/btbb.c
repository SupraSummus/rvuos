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
#include "devregs.h"

/* The baseband's block. */
#define BTBB_BASE 0x600a2000u
/*
 * BB_08 and BB_0C are written by bt_bb_v2_tx_set, BB_18 by bt_bb_v2_tx_set too, BB_10's high byte by
 * bt_bb_set_zb_tx_on_delay and its bits 5 to 12 by bt_bb_set_le_tx_on_delay, BB_B8 by bt_bb_tx_cca_period,
 * and BB_E0 to BB_F4 are the Gaussian filter's coefficients, written by set_gauss_coeff_1m and _2m.
 */
#define BB_08 (BTBB_BASE + 0x008u)
#define BB_0C (BTBB_BASE + 0x00cu)
#define BB_10 (BTBB_BASE + 0x010u)
#define BB_18 (BTBB_BASE + 0x018u)
#define BB_B8 (BTBB_BASE + 0x0b8u)
#define BB_E0 (BTBB_BASE + 0x0e0u)
#define BB_E4 (BTBB_BASE + 0x0e4u)
#define BB_E8 (BTBB_BASE + 0x0e8u)
#define BB_EC (BTBB_BASE + 0x0ecu)
#define BB_F0 (BTBB_BASE + 0x0f0u)
#define BB_F4 (BTBB_BASE + 0x0f4u)
/* The baseband's word 0x14, which btbb_init writes; what it is is not known. */
#define BB_INIT_14 (BTBB_BASE + 0x014u)
/* The word before the baseband's block, 0x600a08fc, which btbb_init writes; what it is is not known. */
#define BB_INIT_18FC 0x600a08fcu

/*
 * The 802.15.4 MAC's block, whose transmit-on delay words ieee802154_txon_delay_set writes.
 * i154.c reaches the same block for its own registers; the base is named in both until they are gathered, see TODO.md.
 */
#define I154_BASE 0x600a3000u
#define I154_104 (I154_BASE + 0x104u)
#define I154_10C (I154_BASE + 0x10cu)
#define I154_110 (I154_BASE + 0x110u)
#define I154_114 (I154_BASE + 0x114u)

/* The archive's functions the bring-up still calls, of libbtbb.a until the driver's own replace them. */
extern void bt_bb_v2_rx_set(uint32_t value);
extern void bt_bb_v2_version(void);
/* The archive's byte the LE transmit-on delay is scaled from, theirs until its cca functions are the driver's too. */
extern uint8_t bt_bb_cca_en;
/* The ROM's, which fills the power table entry by entry; its three arguments' roles are not known. */
extern int get_data_sat(int, int, int);

/* The archive's functions the driver's own take, declared where they are defined below, for the prototype. */
void bt_bb_v2_tx_set(uint32_t mode);
void set_gauss_coeff_1m(void);
void set_gauss_coeff_2m(void);
void bt_bb_set_le_tx_on_delay(uint32_t delay);
void bt_bb_set_zb_tx_on_delay(uint32_t delay);
void bt_bb_tx_cca_period(uint32_t first, uint32_t second);

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
