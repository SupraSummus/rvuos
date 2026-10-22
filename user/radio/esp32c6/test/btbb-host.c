/*
 * What mac-replay-test's bb cases need besides btbb.c on the host: libphy's phy_param, which the driver's own
 * baseband asks and the host answers here. The library's functions the bring-up took are btbb.c's own now,
 * the ROM's get_data_sat among them, so none of them is stubbed, and libbtbb.a is linked no more.
 */

/*
 * libphy's calibration data, of which bt_agc_gain_set reads the byte at 0x120. It is libphy's datum, not a constant of
 * the archive's: this is the value the board's log shows, 0x47, which is all that shows, since each of bt_agc_gain_max's
 * two masks cuts the byte above the seventh bit.
 */
__attribute__((weak)) const unsigned char phy_param[0x121] = { [0x120] = 0x47 };
