# The ESP32-C6's radio, see user/radio/esp32c6/README.md: what every program of it shares,
# the root task, the child page of drv.h, the adapter, the PHY, the basebands and the MACs, and their tests on the host.
# A program is an image of its own that runs as the root task's child from the window onto flash,
# which the loader writes at FLASH_WINDOW_FLASH before the kernel boots, if the flash holds something else there;
# the Wi-Fi station of user/wifi/esp32c6/ is one.
# tools/esp-fetch.py --wifi fetches the PHY's init data and the ROM's linker scripts, never into the tree.
RADIO_ESP      := user/radio/esp32c6
RADIO_ESP_INIT := $(BUILD)/$(RADIO_ESP)/phy_init_data.c
# The radio's own files every program links, and the objects of what is fetched;
# root.c is the root task's own image's, and the sampler's dump.c the root's too.
RADIO_ESP_SRC  := $(filter-out $(RADIO_ESP)/root.c $(RADIO_ESP)/dump.c,$(wildcard $(RADIO_ESP)/*.c))
RADIO_ESP_DATA := $(BUILD)/$(RADIO_ESP)/phy_init_data.o $(BUILD)/$(RADIO_ESP)/esp_wifi_regulatory.o
RADIO_ESP_ROM  := $(addprefix $(PHYBLOB_CACHE)/,esp32c6.rom.ld esp32c6.rom.phy.ld \
                  esp32c6.rom.libgcc.ld esp32c6.rom.libc.ld)
# A program goes where the window onto flash starts, and the configuration to the input region,
# both as the board's headers place them.
RADIO_ESP_AT       := $(call board_hex,FLASH_WINDOW_FLASH,user/board/esp32c6/devices.h)
RADIO_ESP_INPUT_AT := $(call board_hex,INPUT_BASE,kernel/board/esp32c6/board.h)

# The init data is written last, so it stands for every file fetched.
$(RADIO_ESP_INIT): tools/esp-fetch.py
	@mkdir -p $(dir $@)
	tools/esp-fetch.py --cache $(PHYBLOB_CACHE) --init-data $@ --wifi

# libphy.a's functions the radio's own, in phy.c, replace: left out of the link now that the archive is, but still
# weakened in a copy, which wifi-esp32c6-refs and wifi-esp32c6-attrib read as the library's half; the copy is made
# again when this file, which names them, changes.
RADIO_ESP_PHY_OWN := phy_get_xtal_freq tsens_read_init_new tsens_dac_to_index tsens_dac_cal_new \
                     ram_tsens_temp_read_new phy_get_tsens_value tsens_temp_read_new get_temp_init_new \
                     pwdet_reg_init_new open_i2c_xpd_new phy_xpd_rf \
                     phy_wifi_enable_set phy_set_tsens_power phy_xpd_tsens phy_get_max_pwr wait_i2c_sdm_stable \
                     phy_close_rf set_rx_comp_new write_gain_mem_new phy_i2c_master_mem_txcap set_freq_i2c \
                     wifi_get_tx_tab wifi_set_tx_gain bt_get_tx_tab bt_set_tx_gain bt_tx_gain_init \
                     phy_set_most_tpw chan14_mic_cfg wr_rx_gain_mem \
                     chip_v7_set_chan chip_v7_set_chan_offset phy_change_channel \
                     mac_tx_chan_offset_new phy_rx_table_init \
                     i2c_clk_sel i2c_bbpll_set reg_set_new freq_reg_init bias_reg_set \
                     noise_floor_auto_set_new bt_filter_reg_new pbus_clear_reg phy_i2c_init2 filter_dcap_set \
                     freq_get_i2c_data freq_i2c_write_set_new freq_i2c_mem_write_new freq_i2c_data_write_new \
                     rx_11b_opt tx_paon_set_new agc_reg_init_new bb_reg_init_new btbb_wifi_bb_cfg2 phy_reg_init \
                     phy_wakeup_init rf_init bb_init phy_i2c_init1 phy_i2c_master_cmd_mem_init \
                     read_rf_freq_mem_new pll_cap_mem_update_new phy_rfpll_cap_correct_new \
                     phy_rfpll_cap_init_cal_new phy_wait_freq_set_busy_new phy_en_hw_set_freq_new \
                     phy_dis_hw_set_freq_new phy_rfpll_cap_track_new ram_temp_to_power txpwr_cal_track \
                     bt_track_tx_power wifi_track_tx_power tx_i2c_track gen_rx_gain_table_new \
                     phy_force_rx_gain_trig set_rx_gain_cal_dc_new ram_iq_est_enable dc_iq_est_new \
                     dc_iq_est_high rxiq_get_mis_new rfrx_sat_rst phy_check_rx_sat \
                     rxiq_cover_mg_mp_new rfcal_rxiq_new get_rfcal_rxiq_data_new \
                     set_rx_gain_cal_iq_new rxiq_cal_init \
                     tx_cap_init_loop rfcal_txcap_loop \
                     txdc_cal_init_new bt_txdc_cal_new set_lb_txiq_new \
                     register_chipv7_phy_init_param register_chipv7_phy \
                     rfcal_pwrctrl_new tx_pwctrl_init_new tx_pwctrl_init_cal_new bt_tx_pwctrl_init_new phy_set_23dbm \
                     rxdc_est_min_new pbus_rx_dco_cal_1step_new rx_pbus_reset set_rx_gain_table \
                     txdc_cal_pwdet txdc_cal_pwdet_init txdc_cal_new \
                     txiq_cover_new rfcal_txiq_new txiq_cal_init_new phy_cal_param_track force_txrx_off \
                     phy_param_track_tot \
                     phy_get_rf_cal_version phy_get_mac_addr get_iq_value phy_set_mac_data \
                     phy_rfcal_data_sub rf_cal_data_recovery rf_cal_data_backup \
                     phy_rfcal_data_check phy_rfcal_data_check_value \
                     rc_cal_init rc_cal_new i2c_rc_cal_set \
                     wr_rf_freq_mem_new save_pbus_reg write_pbus_mem set_pbus_mem rfpll_chgp_cal \
                     get_rf_freq_init_new set_chan_freq_hw_init \
                     phy_get_romfunc_addr \
                     ram_set_chan_cal_interp ram_bb_bss_cbw40 ram_set_chan_freq_sw_start \
                     ram_get_i2c_read_mask ram_get_i2c_mst0_mask ram_get_i2c_hostid \
                     ram_chip_i2c_readReg_org ram_pbus_force_mode ram_pbus_rd_addr ram_pbus_rd_shift \
                     ram_pbus_xpd_rx_on ram_pbus_xpd_tx_on ram_set_loopback_gain ram_txcal_debuge_mode \
                     ram_pwdet_tone_start ram_pwdet_wait_idle ram_read_sar_dout \
                     ram_start_tx_tone_step ram_stop_tx_tone ram_rfpll_set_freq ram_bb_gain_index \
                     phy_i2c_enter_critical phy_i2c_exit_critical

$(BUILD)/$(RADIO_ESP)/libphy.a: $(RADIO_ESP_INIT) $(RADIO_ESP)/build.mk
	$(OBJCOPY) $(addprefix --weaken-symbol=,$(RADIO_ESP_PHY_OWN)) $(PHYBLOB_CACHE)/libphy.a $@

$(BUILD)/$(RADIO_ESP)/phy_init_data.o: $(RADIO_ESP_INIT)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/$(RADIO_ESP)/esp_wifi_regulatory.o: $(RADIO_ESP_INIT)
	$(CC) $(CFLAGS) -Wno-unterminated-string-initialization -I$(RADIO_ESP)/shim \
		-c $(PHYBLOB_CACHE)/esp_wifi_regulatory.c -o $@

# The root task of every program: root.c, and with C6_SDR=1 the sampler of dump.h, which only that build links;
# and what it shares with the Pico 2 W's, the network process, its clients, and the root task's half of both, system.c,
# which it builds when the program is the Wi-Fi station and a network is named.
# The logger is linked, though this root task never builds it.
RADIO_ESP_NET  := $(addprefix $(BUILD)/user/wifi/,netproc.o net.o sock.o system.o client.o echo.o clock.o sntp.o logger.o)
RADIO_ESP_ROOT := $(BUILD)/$(RADIO_ESP)/root.o $(if $(filter-out 0,$(C6_SDR)),$(BUILD)/$(RADIO_ESP)/dump.o)
$(BUILD)/user-wifi-esp32c6.elf: $(RADIO_ESP_ROOT) $(RADIO_ESP_NET) $(LIB_OBJ) $(USER_COMMON) \
                                $(BUILD)/user/tracer/trace.o $(BUILD)/user/user.ld tools/no-globals.py
	tools/no-globals.py $(RADIO_ESP_NET) $(LIB_OBJ) $(BUILD)/user/tracer/trace.o
	$(CC) $(LDFLAGS) -Wl,-T,$(BUILD)/user/user.ld $(RADIO_ESP_ROOT) $(RADIO_ESP_NET) $(LIB_OBJ) \
		$(BUILD)/user/tracer/trace.o $(USER_COMMON) -o $@

# The radio's files on the host under the sanitizers, which fetch nothing, so make check runs them.
# The sending's choices of txctl.c, each try's rate and backoff.
# The bring-up's register sequences of macstart.c, held to the files of test/replay/, taken once from a run of the
# libraries' start. The libraries are linked no more, so the files are frozen, as bb.txt is below; the make target
# that took them, wifi-esp32c6-replay, is in git history.
RADIO_HOST          := build/host/esp32c6
TXCTL_TEST          := $(RADIO_HOST)/txctl-test
TXCTL_TEST_OBJ      := $(RADIO_HOST)/txctl.o $(RADIO_HOST)/txctl-test.o
$(RADIO_HOST)/txctl.o: $(RADIO_ESP)/txctl.c $(RADIO_ESP)/txctl.h
$(RADIO_HOST)/txctl-test.o: $(RADIO_ESP)/test/txctl-test.c $(RADIO_ESP)/txctl.h
MAC_REPLAY_TEST     := $(RADIO_HOST)/mac-replay-test
MAC_REPLAY_DIR      ?= $(RADIO_ESP)/test/replay
MAC_REPLAY_EXPECTED := $(RADIO_ESP)/test/replay-expected.txt
# The baseband's files apart, so that the Wi-Fi gate, which takes config.txt into a directory of its own, holds it alone.
BTBB_REPLAY_DIR     ?= $(RADIO_ESP)/test/replay
MAC_REPLAY_TEST_OBJ := $(RADIO_HOST)/macstart.o $(RADIO_HOST)/btbb.o $(RADIO_HOST)/btbb-host.o \
                       $(RADIO_HOST)/mac-replay-test.o
$(RADIO_HOST)/macstart.o: $(RADIO_ESP)/macstart.c $(RADIO_ESP)/mac.h $(RADIO_ESP)/macregs.h
$(RADIO_HOST)/btbb.o: $(RADIO_ESP)/btbb.c $(RADIO_ESP)/btbb.h $(RADIO_ESP)/macregs.h user/board/esp32c6/devices.h
$(RADIO_HOST)/btbb-host.o: $(RADIO_ESP)/test/btbb-host.c
$(RADIO_HOST)/mac-replay-test.o: $(RADIO_ESP)/test/mac-replay-test.c $(RADIO_ESP)/mac.h $(RADIO_ESP)/macregs.h \
                                 $(RADIO_ESP)/btbb.h
$(MAC_REPLAY_TEST_OBJ): RADIO_HOST_INC := -DMAC_HOST -Iuser/board/esp32c6

$(TXCTL_TEST_OBJ) $(MAC_REPLAY_TEST_OBJ):
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wshadow $(HOST_SAN) $(RADIO_HOST_INC) -I$(RADIO_ESP) -Iuser \
		-c $< -o $@

$(TXCTL_TEST): $(TXCTL_TEST_OBJ)
	$(HOST_CC) $(HOST_SAN) $^ -o $@

$(MAC_REPLAY_TEST): $(MAC_REPLAY_TEST_OBJ)
	$(HOST_CC) $(HOST_SAN) $^ -o $@

.PHONY: txctl-test mac-replay-test
txctl-test: $(TXCTL_TEST)
	$(TXCTL_TEST)

mac-replay-test: $(MAC_REPLAY_TEST)
	$(MAC_REPLAY_TEST) $(MAC_REPLAY_DIR) config $(MAC_REPLAY_EXPECTED)
	$(MAC_REPLAY_TEST) $(BTBB_REPLAY_DIR) bb
	$(MAC_REPLAY_TEST) $(BTBB_REPLAY_DIR) bb=bb-other
	tests/mac-replay-test.py $(MAC_REPLAY_TEST) $(RADIO_ESP)/test/replay $(MAC_REPLAY_EXPECTED)

# test/replay/bb.txt and bb-other.txt are mac-replay-test's hold on btbb.c: the whole window of libbtbb.a e4897a5's
# bt_bb_v2_init_cmplx, between i154.c's radio_up trace: lines baseband and mac, taken once from a trace=1 i154= run of an
# image whose baseband was still the library's, build/esp32c6/i154/base.elf and base.bin, the driver as of 7dbd462's
# tree, and its lib2.log and lib1.log, the modem's two states. The library is linked no more, so the files are frozen;
# the make target that took them, wifi-esp32c6-bb-replay, is in git history.
#
# test/replay/config.txt is the same for mac_config of macstart.c: the MAC's bring-up groups as the libraries' hal_init
# once wrote them, a step at a time taken into the driver's own. net80211 and pp are linked no more, so it is frozen
# too; wifi-esp32c6-replay, which took it from a trace=1 libstart=1 run, is in git history.
