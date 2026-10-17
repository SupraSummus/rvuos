/*
 * libbtbb.a's functions under bt_bb_v2_init_cmplx, for mac-replay-test's bb case on the host:
 * each does nothing, weakly, so that btbb.c's own takes its place as it is written,
 * and the ones still libbtbb.a's on the board make no access here, as bb.txt holds none of theirs.
 * A function becomes the driver's own by its name leaving BTBB_REPLAY_LIB in build.mk, not this file:
 * btbb.c's strong definition wins at the link either way.
 */

#define LIB(name)                                                                                                       \
    __attribute__((weak)) void name(void)                                                                              \
    {                                                                                                                  \
    }

LIB(set_gauss_coeff_2m)
LIB(set_gauss_coeff_1m)
LIB(ieee802154_txon_delay_set)
LIB(bt_bb_v2_version)
LIB(bt_bb_v2_tx_set)
LIB(zb_rx_v0_0_set)
LIB(bt_bb_rx_set)
LIB(bt_agc_v2_set)
LIB(bt_bb_rx_dpo_set)
LIB(bt_bb_rx_correlator_set)
LIB(bt_agc_target_set)
LIB(bt_agc_restart_set)
LIB(bt_agc_gain_set)
LIB(bt_agc_gain_max)
LIB(bt_agc_detect_set)
LIB(bt_bb_tx_cca_period)
LIB(bt_bb_set_zb_tx_on_delay)
LIB(bt_bb_set_le_tx_on_delay)

/* libbtbb.a's, in its .bss, which bt_bb_tx_cca_en alone sets and the driver never calls, so zero as on the board. */
__attribute__((weak)) unsigned char bt_bb_cca_en;

/* The ROM's get_data_sat, which bt_bb_get_tx_pwr_table asks; the replay never calls it, as btbb_init does not call it. */
__attribute__((weak)) int get_data_sat(int index, int span, int bias)
{
    (void)index;
    (void)span;
    (void)bias;
    return 0;
}

/* libphy.a's, which prints the baseband's version when btbb_init is asked to. */
__attribute__((weak)) int phy_printf(const char *format, ...)
{
    (void)format;
    return 0;
}
