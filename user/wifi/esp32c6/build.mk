# The Wi-Fi system on an ESP32-C6, see user/wifi/esp32c6/drv.h: a root task,
# the driver around Espressif's closed libraries, a child that runs an image of its own from the window onto flash,
# which the loader writes at FLASH_WINDOW_FLASH before the kernel boots, if the flash holds something else there,
# and the network process of user/wifi/ and its clients, children that run the root task's image, so they keep no global.
# tools/wifi-run.py prints the log as the root task writes it to the console, each line with its time,
# checks the system from the host once its clients answer, and ends the run; build/esp32c6/wifi-run.log keeps it,
# and its fault lines name functions from the driver's, the kernel's and the ROM's ELF files.
# tools/esp-fetch.py --wifi fetches the libraries, again never into the tree.
WIFI_ESP      := user/wifi/esp32c6
WIFI_ESP_INIT := $(BUILD)/$(WIFI_ESP)/phy_init_data.c
WIFI_ESP_OBJ  := $(patsubst %.c,$(BUILD)/%.o,$(filter-out $(WIFI_ESP)/root.c,$(wildcard $(WIFI_ESP)/*.c))) \
                 $(BUILD)/$(WIFI_ESP)/phy_init_data.o $(BUILD)/$(WIFI_ESP)/esp_wifi_regulatory.o
# The closed libraries the driver links: the PHY's alone, which keeps the RF; glue.c stands for what libcore.a held.
# The BT baseband, libbtbb.a, was taken whole, see btbb.c, and the Wi-Fi's net80211 and pp with it: the driver's own
# MAC and start stand in their place, so their archives are linked no more.
WIFI_ESP_LIBS := $(BUILD)/$(WIFI_ESP)/libphy.a
WIFI_ESP_ROM  := $(addprefix $(PHYBLOB_CACHE)/,esp32c6.rom.ld esp32c6.rom.phy.ld \
                 esp32c6.rom.libgcc.ld esp32c6.rom.libc.ld)
# The driver goes where the window onto flash starts, and the configuration to the input region,
# both as the board's headers place them.
WIFI_ESP_AT   := $(call board_hex,FLASH_WINDOW_FLASH,user/board/esp32c6/devices.h)
# WIFI_CONFIG names the network to join, ssid=, pass= and bssid=, from a file git does not track, as for the Pico 2 W,
# and how long the run lasts, run= seconds, after which the root task halts, and the log comes out; see root.c.
# With none the input region gets an empty one, since the board's RAM keeps the last run's.
WIFI_ESP_INPUT_AT := $(call board_hex,INPUT_BASE,kernel/board/esp32c6/board.h)

# The init data is written last, so it stands for every file fetched.
$(WIFI_ESP_INIT): tools/esp-fetch.py
	@mkdir -p $(dir $@)
	tools/esp-fetch.py --cache $(PHYBLOB_CACHE) --init-data $@ --wifi

# hostap's supplicant, the parts of wpa_supplicant's src/ that tools/esp-fetch.py --wifi unpacks:
# the 4-way and group key handshakes of rsn_supp/, WPA3's SAE of common/, and the crypto of hostap's own they run on,
# but for MD5, whose md5_vector esp32c6.rom.ld gives from the ROM.
# hostap's files, and the driver's that read its headers, take hostap/includes.h first, for the C library hostap expects;
# hostap's own warnings are its own, so they are not the build's errors.
HOSTAP      := $(PHYBLOB_CACHE)/hostap/src
HOSTAP_SRC  := rsn_supp/wpa.c rsn_supp/wpa_ie.c rsn_supp/pmksa_cache.c \
               common/wpa_common.c common/ieee802_11_common.c common/sae.c common/dragonfly.c \
               utils/common.c utils/wpabuf.c \
               crypto/sha1.c crypto/sha1-internal.c crypto/sha1-prf.c crypto/sha1-pbkdf2.c \
               crypto/sha256.c crypto/sha256-internal.c crypto/sha256-prf.c crypto/sha256-kdf.c crypto/md5.c \
               crypto/aes-internal.c crypto/aes-internal-enc.c crypto/aes-internal-dec.c \
               crypto/aes-wrap.c crypto/aes-unwrap.c crypto/aes-omac1.c crypto/aes-cbc.c crypto/aes-ccm.c
HOSTAP_OBJ  := $(patsubst %.c,$(BUILD)/hostap/%.o,$(HOSTAP_SRC))
HOSTAP_INC  := -include $(WIFI_ESP)/hostap/includes.h -isystem $(HOSTAP) -isystem $(HOSTAP)/utils
WIFI_ESP_OBJ += $(HOSTAP_OBJ)

# The fault tracer's decoder and the carries-out of a served access, user/tracer/, in the driver's image:
# the decoder is host-tested too, by user/tracer/build.mk, and the two are the same object there and here.
WIFI_ESP_TRACE := $(BUILD)/user/tracer/trace.o $(BUILD)/user/tracer/watch.o
WIFI_ESP_OBJ   += $(WIFI_ESP_TRACE)

$(BUILD)/hostap/%.o: $(WIFI_ESP)/hostap/includes.h $(WIFI_ESP_INIT)
	@mkdir -p $(dir $@)
	$(CC) $(ARCHFLAGS) -std=gnu11 -ffreestanding -fno-builtin -fno-pic -fno-common -O2 -g \
		-ffunction-sections -fdata-sections -w $(HOSTAP_INC) -c $(HOSTAP)/$*.c -o $@

# Mbed TLS's big numbers and P-256, which ec.c gives SAE as hostap's crypto, see mbedtls/config.h:
# the parts of its library/ that tools/esp-fetch.py --wifi unpacks, built with no system headers,
# only the few of a C library mbedtls/libc/ declares; its warnings are its own too.
MBEDTLS     := $(PHYBLOB_CACHE)/mbedtls
MBEDTLS_SRC := library/bignum.c library/bignum_core.c library/ecp.c library/ecp_curves.c \
               library/constant_time.c library/platform_util.c
MBEDTLS_OBJ := $(patsubst %.c,$(BUILD)/mbedtls/%.o,$(MBEDTLS_SRC))
MBEDTLS_CFG := -DMBEDTLS_CONFIG_FILE='"config.h"' -I$(WIFI_ESP)/mbedtls -isystem $(MBEDTLS)/include
MBEDTLS_INC := $(MBEDTLS_CFG) -isystem $(WIFI_ESP)/mbedtls/libc
WIFI_ESP_OBJ += $(MBEDTLS_OBJ)

$(BUILD)/mbedtls/%.o: $(wildcard $(WIFI_ESP)/mbedtls/*.h $(WIFI_ESP)/mbedtls/libc/*.h) $(WIFI_ESP_INIT)
	@mkdir -p $(dir $@)
	$(CC) $(ARCHFLAGS) -std=c11 -ffreestanding -fno-builtin -fno-pic -fno-common -nostdlibinc -O2 -g \
		-ffunction-sections -fdata-sections -w $(MBEDTLS_INC) -c $(MBEDTLS)/$*.c -o $@

$(addprefix $(BUILD)/$(WIFI_ESP)/,supp.o hostap.o ec.o mgmt.o ccmp.o): CFLAGS += $(HOSTAP_INC)
$(addprefix $(BUILD)/$(WIFI_ESP)/,supp.o hostap.o ec.o mgmt.o ccmp.o): $(WIFI_ESP_INIT)
$(BUILD)/$(WIFI_ESP)/ec.o: CFLAGS += $(MBEDTLS_INC)

# The driver's files that work with hostap's, on the host under the sanitizers, with hostap's own system functions:
# WPA3's SAE, hostap's sae.c and dragonfly.c over ec.c and Mbed TLS,
# against IEEE 802.11's test vectors and whole exchanges, test/sae-test.c,
# the station's management frames, mgmt.c over hostap's parser of elements, test/mgmt-test.c,
# and its CCMP, ccmp.c over hostap's CCM, against IEEE 802.11's own test vector, test/ccmp-test.c.
# They build what tools/esp-fetch.py fetches, so make check, which fetches nothing, leaves them out,
# and make BOARD=esp32c6 wifi-esp32c6 runs them before the board.
ESP_HOST        := build/host/esp32c6
ESP_HOST_DEF    := -DCONFIG_SAE -DCONFIG_SHA256 -DCONFIG_CRYPTO_INTERNAL -DCONFIG_NO_RANDOM_POOL
ESP_HOST_INC    := $(ESP_HOST_DEF) -isystem $(HOSTAP) -isystem $(HOSTAP)/utils $(MBEDTLS_CFG)
ESP_HOST_UTILS  := utils/common.c utils/wpabuf.c utils/os_unix.c utils/wpa_debug.c
SAE_TEST        := $(ESP_HOST)/sae-test
SAE_TEST_SRC    := common/sae.c common/dragonfly.c common/wpa_common.c $(ESP_HOST_UTILS) \
                   $(filter crypto/%,$(HOSTAP_SRC)) crypto/md5-internal.c
SAE_TEST_OBJ    := $(patsubst %.c,$(ESP_HOST)/hostap/%.o,$(SAE_TEST_SRC)) \
                   $(patsubst %.c,$(ESP_HOST)/mbedtls/%.o,$(MBEDTLS_SRC)) \
                   $(ESP_HOST)/ec.o $(ESP_HOST)/sae-test.o
MGMT_TEST       := $(ESP_HOST)/mgmt-test
MGMT_TEST_OBJ   := $(patsubst %.c,$(ESP_HOST)/hostap/%.o,common/ieee802_11_common.c \
                   crypto/aes-omac1.c crypto/aes-internal.c crypto/aes-internal-enc.c $(ESP_HOST_UTILS)) \
                   $(ESP_HOST)/mgmt.o $(ESP_HOST)/mgmt-test.o
CCMP_TEST       := $(ESP_HOST)/ccmp-test
CCMP_TEST_OBJ   := $(patsubst %.c,$(ESP_HOST)/hostap/%.o,crypto/aes-ccm.c crypto/aes-internal.c \
                   crypto/aes-internal-enc.c $(ESP_HOST_UTILS)) $(ESP_HOST)/ccmp.o $(ESP_HOST)/ccmp-test.o
# The station's key transitions of keys.c hold nothing fetched, so their test is a part of `make check` too;
# the tests above are left out of it, as they fetch hostap and Mbed TLS. See CLAUDE.md.
KEYS_TEST       := $(ESP_HOST)/keys-test
KEYS_TEST_OBJ   := $(ESP_HOST)/keys.o $(ESP_HOST)/keys-test.o
$(ESP_HOST)/keys.o: $(WIFI_ESP)/keys.c $(WIFI_ESP)/keys.h
$(ESP_HOST)/keys-test.o: $(WIFI_ESP)/test/keys-test.c $(WIFI_ESP)/keys.h
# The sending's choices of txctl.c, each try's rate and backoff, likewise.
TXCTL_TEST      := $(ESP_HOST)/txctl-test
TXCTL_TEST_OBJ  := $(ESP_HOST)/txctl.o $(ESP_HOST)/txctl-test.o
$(ESP_HOST)/txctl.o: $(WIFI_ESP)/txctl.c $(WIFI_ESP)/txctl.h
$(ESP_HOST)/txctl-test.o: $(WIFI_ESP)/test/txctl-test.c $(WIFI_ESP)/txctl.h
# The bring-up's register sequences of macstart.c, held to the files of test/replay/, taken once from a run of the
# libraries' start; they fetch nothing, so make check runs them. The libraries are linked no more, so the files are
# frozen, as bb.txt is below; the make target that took them, wifi-esp32c6-replay, is in git history.
MAC_REPLAY_TEST     := $(ESP_HOST)/mac-replay-test
MAC_REPLAY_DIR      ?= $(WIFI_ESP)/test/replay
MAC_REPLAY_EXPECTED := $(WIFI_ESP)/test/replay-expected.txt
# The baseband's files apart, so that the Wi-Fi gate, which takes config.txt into a directory of its own, holds it alone.
BTBB_REPLAY_DIR     ?= $(WIFI_ESP)/test/replay
MAC_REPLAY_TEST_OBJ := $(ESP_HOST)/macstart.o $(ESP_HOST)/btbb.o $(ESP_HOST)/btbb-host.o $(ESP_HOST)/mac-replay-test.o
$(ESP_HOST)/macstart.o: $(WIFI_ESP)/macstart.c $(WIFI_ESP)/mac.h $(WIFI_ESP)/macregs.h
$(ESP_HOST)/btbb.o: $(WIFI_ESP)/btbb.c $(WIFI_ESP)/btbb.h $(WIFI_ESP)/macregs.h user/board/esp32c6/devices.h
$(ESP_HOST)/btbb-host.o: $(WIFI_ESP)/test/btbb-host.c
$(ESP_HOST)/mac-replay-test.o: $(WIFI_ESP)/test/mac-replay-test.c $(WIFI_ESP)/mac.h $(WIFI_ESP)/macregs.h \
                               $(WIFI_ESP)/btbb.h
$(MAC_REPLAY_TEST_OBJ): ESP_HOST_INC += -DMAC_HOST -Iuser/board/esp32c6

$(ESP_HOST)/hostap/%.o: $(WIFI_ESP_INIT)
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=gnu11 -O1 -g $(HOST_SAN) -w $(ESP_HOST_INC) -c $(HOSTAP)/$*.c -o $@

# hostap's CCM reads the message in words, so a QoS frame's payload, two bytes past a word, trips the host's
# alignment check; off for this one fetched file, on for the driver's own and its tests.
$(ESP_HOST)/hostap/crypto/aes-ccm.o: HOST_SAN += -fno-sanitize=alignment

$(ESP_HOST)/mbedtls/%.o: $(wildcard $(WIFI_ESP)/mbedtls/*.h) $(WIFI_ESP_INIT)
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -O1 -g $(HOST_SAN) -w $(MBEDTLS_CFG) -c $(MBEDTLS)/$*.c -o $@

$(ESP_HOST)/ec.o: $(WIFI_ESP)/ec.c $(wildcard $(WIFI_ESP)/mbedtls/*.h) $(WIFI_ESP_INIT)
$(ESP_HOST)/sae-test.o: $(WIFI_ESP)/test/sae-test.c $(WIFI_ESP_INIT)
$(ESP_HOST)/mgmt.o: $(WIFI_ESP)/mgmt.c $(WIFI_ESP)/mgmt.h $(WIFI_ESP_INIT)
$(ESP_HOST)/mgmt-test.o: $(WIFI_ESP)/test/mgmt-test.c $(WIFI_ESP)/mgmt.h $(WIFI_ESP_INIT)
$(ESP_HOST)/ccmp.o: $(WIFI_ESP)/ccmp.c $(WIFI_ESP)/ccmp.h $(WIFI_ESP_INIT)
$(ESP_HOST)/ccmp-test.o: $(WIFI_ESP)/test/ccmp-test.c $(WIFI_ESP)/ccmp.h $(WIFI_ESP_INIT)
$(addprefix $(ESP_HOST)/,ec.o sae-test.o mgmt.o mgmt-test.o ccmp.o ccmp-test.o keys.o keys-test.o) \
		$(TXCTL_TEST_OBJ) $(MAC_REPLAY_TEST_OBJ):
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wshadow $(HOST_SAN) $(ESP_HOST_INC) -I$(WIFI_ESP) -Iuser \
		-c $< -o $@

$(SAE_TEST): $(SAE_TEST_OBJ)
	$(HOST_CC) $(HOST_SAN) $^ -o $@

$(MGMT_TEST): $(MGMT_TEST_OBJ)
	$(HOST_CC) $(HOST_SAN) $^ -o $@

$(CCMP_TEST): $(CCMP_TEST_OBJ)
	$(HOST_CC) $(HOST_SAN) $^ -o $@

$(KEYS_TEST): $(KEYS_TEST_OBJ)
	$(HOST_CC) $(HOST_SAN) $^ -o $@

$(TXCTL_TEST): $(TXCTL_TEST_OBJ)
	$(HOST_CC) $(HOST_SAN) $^ -o $@

$(MAC_REPLAY_TEST): $(MAC_REPLAY_TEST_OBJ)
	$(HOST_CC) $(HOST_SAN) $^ -o $@

.PHONY: sae-test mgmt-test ccmp-test keys-test txctl-test mac-replay-test
sae-test: $(SAE_TEST)
	$(SAE_TEST)

mgmt-test: $(MGMT_TEST)
	$(MGMT_TEST)

ccmp-test: $(CCMP_TEST)
	$(CCMP_TEST)

keys-test: $(KEYS_TEST)
	$(KEYS_TEST)

txctl-test: $(TXCTL_TEST)
	$(TXCTL_TEST)

mac-replay-test: $(MAC_REPLAY_TEST)
	$(MAC_REPLAY_TEST) $(MAC_REPLAY_DIR) config $(MAC_REPLAY_EXPECTED)
	$(MAC_REPLAY_TEST) $(BTBB_REPLAY_DIR) bb
	$(MAC_REPLAY_TEST) $(BTBB_REPLAY_DIR) bb=bb-other
	tests/mac-replay-test.py $(MAC_REPLAY_TEST) $(WIFI_ESP)/test/replay $(MAC_REPLAY_EXPECTED)

# libphy.a's functions the driver's own, in phy.c, replace: weakened in a copy, so that the driver's take their
# place; the copy is made again when this file, which names them, changes.
WIFI_ESP_PHY_OWN := phy_get_xtal_freq tsens_read_init_new pwdet_reg_init_new open_i2c_xpd_new phy_xpd_rf \
                    phy_wifi_enable_set phy_set_tsens_power phy_xpd_tsens phy_get_max_pwr wait_i2c_sdm_stable \
                    phy_close_rf set_rx_comp_new write_gain_mem_new phy_i2c_master_mem_txcap set_freq_i2c \
                    wifi_get_tx_tab wifi_set_tx_gain phy_set_most_tpw chan14_mic_cfg wr_rx_gain_mem \
                    chip_v7_set_chan phy_change_channel \
                    i2c_clk_sel i2c_bbpll_set reg_set_new freq_reg_init bias_reg_set \
                    noise_floor_auto_set_new bt_filter_reg_new pbus_clear_reg phy_i2c_init2 filter_dcap_set \
                    freq_get_i2c_data freq_i2c_write_set_new freq_i2c_mem_write_new freq_i2c_data_write_new \
                    rx_11b_opt tx_paon_set_new agc_reg_init_new bb_reg_init_new btbb_wifi_bb_cfg2 phy_reg_init \
                    phy_wakeup_init read_rf_freq_mem_new pll_cap_mem_update_new phy_rfpll_cap_correct_new \
                    phy_rfpll_cap_init_cal_new phy_wait_freq_set_busy_new phy_en_hw_set_freq_new \
                    phy_dis_hw_set_freq_new phy_rfpll_cap_track_new ram_temp_to_power txpwr_cal_track \
                    bt_track_tx_power wifi_track_tx_power tx_i2c_track gen_rx_gain_table_new \
                    phy_force_rx_gain_trig set_rx_gain_cal_dc_new ram_iq_est_enable dc_iq_est_new \
                    rxdc_est_min_new pbus_rx_dco_cal_1step_new rx_pbus_reset set_rx_gain_table \
                    txdc_cal_pwdet txdc_cal_pwdet_init

$(BUILD)/$(WIFI_ESP)/libphy.a: $(WIFI_ESP_INIT) $(WIFI_ESP)/build.mk
	$(OBJCOPY) $(addprefix --weaken-symbol=,$(WIFI_ESP_PHY_OWN)) $(PHYBLOB_CACHE)/libphy.a $@

$(BUILD)/$(WIFI_ESP)/phy_init_data.o: $(WIFI_ESP_INIT)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/$(WIFI_ESP)/esp_wifi_regulatory.o: $(WIFI_ESP_INIT)
	$(CC) $(CFLAGS) -Wno-unterminated-string-initialization -I$(WIFI_ESP)/shim \
		-c $(PHYBLOB_CACHE)/esp_wifi_regulatory.c -o $@

# The driver's own code takes no memory as it runs, see osi.h, so the link fails if an object but the glue
# of the foreign code calls an allocator: hostap.c, supp.c and ec.c for hostap's and Mbed TLS's, osi.c for the libraries'.
WIFI_ESP_HEAP := $(addprefix $(BUILD)/$(WIFI_ESP)/,ec.o heap.o hostap.o osi.o supp.o)

$(BUILD)/wifi-drv.elf: $(WIFI_ESP_OBJ) $(LIB_OBJ) $(BUILD)/$(WIFI_ESP)/flash.ld $(BUILD)/$(WIFI_ESP)/libphy.a
	@! $(NM) -u $(filter-out $(WIFI_ESP_HEAP),$(filter $(BUILD)/$(WIFI_ESP)/%,$(WIFI_ESP_OBJ))) | grep -E ' (osi_(m|c|re)alloc|os_(m|z)alloc|malloc|calloc)$$' \
		|| { echo "$@: the driver's own code calls an allocator" >&2; exit 1; }
	$(CC) $(LDFLAGS) -Wl,-T,$(BUILD)/$(WIFI_ESP)/flash.ld $(foreach s,$(WIFI_ESP_ROM),-Wl,-T,$(s)) \
		$(WIFI_ESP_OBJ) $(LIB_OBJ) -Wl,--start-group $(WIFI_ESP_LIBS) -Wl,--end-group -o $@

$(BUILD)/wifi-drv.bin: $(BUILD)/wifi-drv.elf
	$(OBJCOPY) -O binary $< $@

# What it shares with the Pico 2 W's: the network process, its clients, and the root task's half of both, system.c;
# the logger is linked, though this root task never builds it.
WIFI_ESP_NET := $(addprefix $(BUILD)/user/wifi/,netproc.o net.o sock.o system.o client.o echo.o clock.o sntp.o logger.o)
$(BUILD)/user-wifi-esp32c6.elf: $(BUILD)/$(WIFI_ESP)/root.o $(WIFI_ESP_NET) $(LIB_OBJ) $(USER_COMMON) \
                                $(BUILD)/user/tracer/trace.o $(BUILD)/user/user.ld tools/no-globals.py
	tools/no-globals.py $(WIFI_ESP_NET) $(LIB_OBJ) $(BUILD)/user/tracer/trace.o
	$(CC) $(LDFLAGS) -Wl,-T,$(BUILD)/user/user.ld $(BUILD)/$(WIFI_ESP)/root.o $(WIFI_ESP_NET) $(LIB_OBJ) \
		$(BUILD)/user/tracer/trace.o $(USER_COMMON) -o $@

.PHONY: wifi-esp32c6
ifeq ($(BOARD),esp32c6)
wifi-esp32c6: $(BUILD)/kernel-wifi-esp32c6.bin $(BUILD)/wifi-drv.bin $(SAE_TEST) $(MGMT_TEST) $(CCMP_TEST)
	$(SAE_TEST)
	$(MGMT_TEST)
	$(CCMP_TEST)
	tools/wifi-run.py --console --save $(BUILD)/wifi-run.log \
		$(foreach e,$(BUILD)/wifi-drv.elf $(BUILD)/kernel-wifi-esp32c6.elf $(PHYBLOB_ROM_ELF),--symbols $(e)) -- \
		$(ESPTOOL_PYTHON) tools/esp32c6-run.py --port $(PORT) --flash $(WIFI_ESP_AT):$(BUILD)/wifi-drv.bin \
		--text $(WIFI_ESP_INPUT_AT):$(or $(WIFI_CONFIG),/dev/null) $(BUILD)/kernel-wifi-esp32c6.bin
else
wifi-esp32c6:
	$(error the driver runs on an ESP32-C6: make BOARD=esp32c6 wifi-esp32c6)
endif

# The medium shared with a Pico 2 W at the C6's access point, see tools/wifi-share.py:
# PICO_CONFIG names the Pico's configuration, as WIFI_CONFIG the C6's, each with the access point's bssid and a run=
# that outlasts the rounds, and SHARE_BEFORE a driver image from before a change, held against this one.
# Both boot with the kernel image at hand, whose root task lays out the driver's page: an image from before a change
# to drv.h's page wants the kernel image built with it, each boot then given to tools/wifi-share.py as its own --c6.
.PHONY: wifi-esp32c6-share
ifeq ($(BOARD),esp32c6)
WIFI_ESP_BOOT = $(ESPTOOL_PYTHON) tools/esp32c6-run.py --port $(PORT) --flash $(WIFI_ESP_AT):$(1) \
	--text $(WIFI_ESP_INPUT_AT):$(WIFI_CONFIG) $(BUILD)/kernel-wifi-esp32c6.bin
wifi-esp32c6-share: $(BUILD)/kernel-wifi-esp32c6.bin $(BUILD)/wifi-drv.bin
	tools/wifi-share.py --pico "make BOARD=rp2350 wifi WIFI_CONFIG=$(PICO_CONFIG)" \
		--c6 "$(call WIFI_ESP_BOOT,$(BUILD)/wifi-drv.bin)" $(if $(SHARE_BEFORE),--c6 "$(call WIFI_ESP_BOOT,$(SHARE_BEFORE))")
else
wifi-esp32c6-share:
	$(error the driver runs on an ESP32-C6: make BOARD=esp32c6 wifi-esp32c6-share)
endif

# The device registers the driver reaches, as `make phymap` lists the PHY harness's, see user/phyblob/build.mk,
# but from every function of its image,
# since it reaches the libraries' tasks and their interrupt's handler only through pointers.
# The board's frames are the driver's, and the eFuse's, which the root task reads for it.
.PHONY: wifi-esp32c6-map
ifeq ($(BOARD),esp32c6)
wifi-esp32c6-map: $(BUILD)/wifi-drv.elf $(PHYBLOB_ROM_ELF)
	tools/phymap.py --objdump $(OBJDUMP) --all $(BUILD)/wifi-drv.elf $(PHYBLOB_ROM_ELF) kernel/board/esp32c6/board.h
else
wifi-esp32c6-map:
	$(error the driver runs on an ESP32-C6: make BOARD=esp32c6 wifi-esp32c6-map)
endif

# What the driver's own objects reach of the closed library it still links, the PHY's,
# each with the ROM's half of it, see tools/esp-refs.py:
# the count a commit that drops a call into it gives in its message, before and after, as CLAUDE.md asks.
# net80211 and pp are gone; their counts read 0 at the commit that took them out, see the commit message.
WIFI_ESP_OWN := $(filter $(BUILD)/$(WIFI_ESP)/%,$(WIFI_ESP_OBJ))
.PHONY: wifi-esp32c6-refs
ifeq ($(BOARD),esp32c6)
wifi-esp32c6-refs: $(WIFI_ESP_OWN) $(BUILD)/$(WIFI_ESP)/libphy.a
	tools/esp-refs.py --nm $(NM) \
		--lib phy=$(BUILD)/$(WIFI_ESP)/libphy.a:$(PHYBLOB_CACHE)/esp32c6.rom.phy.ld $(WIFI_ESP_OWN)
else
wifi-esp32c6-refs:
	$(error the driver runs on an ESP32-C6: make BOARD=esp32c6 wifi-esp32c6-refs)
endif

# The library and function each device access of a trace log belongs to, see tools/mac-trace.py:
# what of the log the own bring-up wrote, and what is libphy's calibration.
# The log has to be the one the attributed image ran, which the rule holds by the log's own flashed-image hash.
# WIFI_TRACE_ONLY=phy holds libphy's part alone, and refuses a log short beside it; a loss is a fault, not the channel.
# WIFI_TRACE_SEGMENTS=1 splits the log by the trace: lines' request numbers, the calls framing each segment; a
# short log is refused, since a loss shifts every boundary past it. WIFI_TRACE_TOP names that many functions
# under each segment (0 names all); the default is 8.
WIFI_TRACE_LOG ?= $(BUILD)/wifi-run.log
.PHONY: wifi-esp32c6-attrib
ifeq ($(BOARD),esp32c6)
wifi-esp32c6-attrib: $(BUILD)/wifi-drv.elf $(BUILD)/wifi-drv.bin $(PHYBLOB_ROM_ELF)
	tools/mac-trace.py attrib --elf $(BUILD)/wifi-drv.elf --rom $(PHYBLOB_ROM_ELF) --image $(BUILD)/wifi-drv.bin \
		--lib phy=$(BUILD)/$(WIFI_ESP)/libphy.a:$(PHYBLOB_CACHE)/esp32c6.rom.phy.ld \
		$(addprefix --own ,$(WIFI_ESP_OBJ) $(LIB_OBJ)) \
		$(if $(WIFI_TRACE_ONLY),--only $(WIFI_TRACE_ONLY),) $(if $(WIFI_TRACE_SEGMENTS),--segments,) \
		$(if $(WIFI_TRACE_TOP),--top $(WIFI_TRACE_TOP),) $(WIFI_TRACE_LOG)
else
wifi-esp32c6-attrib:
	$(error the driver runs on an ESP32-C6: make BOARD=esp32c6 wifi-esp32c6-attrib)
endif

# test/replay/bb.txt and bb-other.txt are mac-replay-test's hold on btbb.c: the whole window of libbtbb.a e4897a5's
# bt_bb_v2_init_cmplx, between i154.c's radio_up trace: lines baseband and mac, taken once from a trace=1 i154= run of an
# image whose baseband was still the library's, build/esp32c6/i154/base.elf and base.bin, the driver as of 7dbd462's
# tree, and its lib2.log and lib1.log, the modem's two states. The library is linked no more, so the files are frozen;
# the make target that took them, wifi-esp32c6-bb-replay, is in git history.
#
# test/replay/config.txt is the same for mac_config of macstart.c: the MAC's bring-up groups as the libraries' hal_init
# once wrote them, a step at a time taken into the driver's own. net80211 and pp are linked no more, so it is frozen
# too; wifi-esp32c6-replay, which took it from a trace=1 libstart=1 run, is in git history.

# Every check of a step of the own start, as CLAUDE.md names them, in their order, on the board, each verdict told;
# see tools/esp32c6-gate.py.
# WIFI_GATE_JOINS names the networks to join, from files git does not track, as WIFI_CONFIG does.
WIFI_GATE_JOINS ?=
.PHONY: wifi-esp32c6-gate
ifeq ($(BOARD),esp32c6)
wifi-esp32c6-gate:
	tools/esp32c6-gate.py $(addprefix --join ,$(WIFI_GATE_JOINS))
else
wifi-esp32c6-gate:
	$(error the driver runs on an ESP32-C6: make BOARD=esp32c6 wifi-esp32c6-gate)
endif

# The own start's accesses to the MAC in a trace=1 run of the build at hand, WIFI_TRACE_OWN, held to the gated run's,
# WIFI_TRACE_GATED, value by value: the check of a change meant to leave the start's device work as the gate held it.
WIFI_TRACE_GATED ?= $(BUILD)/gate/own.log
.PHONY: wifi-esp32c6-seq
wifi-esp32c6-seq:
	tools/mac-trace.py seq $(WIFI_TRACE_GATED) $(or $(WIFI_TRACE_OWN),$(BUILD)/wifi-run.log)
