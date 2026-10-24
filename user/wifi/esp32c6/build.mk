# The Wi-Fi system on an ESP32-C6, see user/radio/esp32c6/drv.h: the radio's root task, see user/radio/esp32c6/build.mk,
# the driver, this directory's station over the radio's PHY and MAC, a program of the radio's that runs from the window
# onto flash, and the network process of user/wifi/ and its clients, children that run the root task's image,
# so they keep no global.
# tools/wifi-run.py prints the log as the root task writes it to the console, each line with its time,
# checks the system from the host once its clients answer, and ends the run; build/esp32c6/wifi-run.log keeps it,
# and its fault lines name functions from the driver's, the kernel's and the ROM's ELF files.
# tools/esp-fetch.py --wifi fetches hostap and Mbed TLS with the radio's files, again never into the tree.
WIFI_ESP      := user/wifi/esp32c6
# The station's files and the radio's, in the order of their names, as the link has always taken them.
WIFI_ESP_SRC  := $(wildcard $(WIFI_ESP)/*.c) $(RADIO_ESP_SRC)
WIFI_ESP_OBJ  := $(patsubst %.c,$(BUILD)/%.o,$(foreach n,$(sort $(notdir $(WIFI_ESP_SRC))),$(filter %/$(n),$(WIFI_ESP_SRC)))) \
                 $(RADIO_ESP_DATA)
# The closed libraries the driver links: none. The PHY's was the last: its functions are the radio's own and its
# two objects, phy_param and g_phyFuns, are in phy.c, so libphy.a is out of the link. The BT baseband, libbtbb.a, was
# taken whole, see btbb.c, and the Wi-Fi's net80211 and pp with it: the radio's own MAC and start stand in their
# place. glue.c stands for what libcore.a held.
WIFI_ESP_LIBS :=
# The station's files include the radio's headers.
$(BUILD)/$(WIFI_ESP)/%.o: CFLAGS += -I$(RADIO_ESP)
# WIFI_CONFIG names the network to join, ssid=, pass= and bssid=, from a file git does not track, as for the Pico 2 W,
# and how long the run lasts, run= seconds, after which the root task halts, and the log comes out; see root.c.
# With none the input region gets an empty one, since the flash keeps the last run's.

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

$(BUILD)/hostap/%.o: $(WIFI_ESP)/hostap/includes.h $(RADIO_ESP_INIT)
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

$(BUILD)/mbedtls/%.o: $(wildcard $(WIFI_ESP)/mbedtls/*.h $(WIFI_ESP)/mbedtls/libc/*.h) $(RADIO_ESP_INIT)
	@mkdir -p $(dir $@)
	$(CC) $(ARCHFLAGS) -std=c11 -ffreestanding -fno-builtin -fno-pic -fno-common -nostdlibinc -O2 -g \
		-ffunction-sections -fdata-sections -w $(MBEDTLS_INC) -c $(MBEDTLS)/$*.c -o $@

$(addprefix $(BUILD)/$(WIFI_ESP)/,supp.o hostap.o ec.o mgmt.o ccmp.o): CFLAGS += $(HOSTAP_INC)
$(addprefix $(BUILD)/$(WIFI_ESP)/,supp.o hostap.o ec.o mgmt.o ccmp.o): $(RADIO_ESP_INIT)
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
$(ESP_HOST)/hostap/%.o: $(RADIO_ESP_INIT)
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=gnu11 -O1 -g $(HOST_SAN) -w $(ESP_HOST_INC) -c $(HOSTAP)/$*.c -o $@

# hostap's CCM reads the message in words, so a QoS frame's payload, two bytes past a word, trips the host's
# alignment check; off for this one fetched file, on for the driver's own and its tests.
$(ESP_HOST)/hostap/crypto/aes-ccm.o: HOST_SAN += -fno-sanitize=alignment

$(ESP_HOST)/mbedtls/%.o: $(wildcard $(WIFI_ESP)/mbedtls/*.h) $(RADIO_ESP_INIT)
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -O1 -g $(HOST_SAN) -w $(MBEDTLS_CFG) -c $(MBEDTLS)/$*.c -o $@

$(ESP_HOST)/ec.o: $(WIFI_ESP)/ec.c $(wildcard $(WIFI_ESP)/mbedtls/*.h) $(RADIO_ESP_INIT)
$(ESP_HOST)/sae-test.o: $(WIFI_ESP)/test/sae-test.c $(RADIO_ESP_INIT)
$(ESP_HOST)/mgmt.o: $(WIFI_ESP)/mgmt.c $(WIFI_ESP)/mgmt.h $(RADIO_ESP_INIT)
$(ESP_HOST)/mgmt-test.o: $(WIFI_ESP)/test/mgmt-test.c $(WIFI_ESP)/mgmt.h $(RADIO_ESP_INIT)
$(ESP_HOST)/ccmp.o: $(WIFI_ESP)/ccmp.c $(WIFI_ESP)/ccmp.h $(RADIO_ESP_INIT)
$(ESP_HOST)/ccmp-test.o: $(WIFI_ESP)/test/ccmp-test.c $(WIFI_ESP)/ccmp.h $(RADIO_ESP_INIT)
$(addprefix $(ESP_HOST)/,ec.o sae-test.o mgmt.o mgmt-test.o ccmp.o ccmp-test.o keys.o keys-test.o):
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

.PHONY: sae-test mgmt-test ccmp-test keys-test
sae-test: $(SAE_TEST)
	$(SAE_TEST)

mgmt-test: $(MGMT_TEST)
	$(MGMT_TEST)

ccmp-test: $(CCMP_TEST)
	$(CCMP_TEST)

keys-test: $(KEYS_TEST)
	$(KEYS_TEST)

# The driver's own code takes no memory as it runs, see osi.h, so the link fails if an object but the glue
# of the foreign code calls an allocator: hostap.c, supp.c and ec.c for hostap's and Mbed TLS's, osi.c for
# the adapter's traced run, and heap.c for the allocator itself.
WIFI_ESP_HEAP := $(addprefix $(BUILD)/$(WIFI_ESP)/,ec.o hostap.o supp.o) $(addprefix $(BUILD)/$(RADIO_ESP)/,heap.o osi.o)

$(BUILD)/wifi-drv.elf: $(WIFI_ESP_OBJ) $(LIB_OBJ) $(BUILD)/$(RADIO_ESP)/flash.ld
	@! $(NM) -u $(filter-out $(WIFI_ESP_HEAP),$(filter $(BUILD)/$(WIFI_ESP)/% $(BUILD)/$(RADIO_ESP)/%,$(WIFI_ESP_OBJ))) | grep -E ' (osi_(m|c|re)alloc|os_(m|z)alloc|malloc|calloc)$$' \
		|| { echo "$@: the driver's own code calls an allocator" >&2; exit 1; }
	$(CC) $(LDFLAGS) -Wl,-T,$(BUILD)/$(RADIO_ESP)/flash.ld $(foreach s,$(RADIO_ESP_ROM),-Wl,-T,$(s)) \
		$(WIFI_ESP_OBJ) $(LIB_OBJ) -Wl,--start-group $(WIFI_ESP_LIBS) -Wl,--end-group -o $@

$(BUILD)/wifi-drv.bin: $(BUILD)/wifi-drv.elf
	$(OBJCOPY) -O binary $< $@


.PHONY: wifi-esp32c6
ifeq ($(BOARD),esp32c6)
wifi-esp32c6: $(BUILD)/kernel-wifi-esp32c6.bin $(BUILD)/wifi-drv.bin $(SAE_TEST) $(MGMT_TEST) $(CCMP_TEST)
	$(SAE_TEST)
	$(MGMT_TEST)
	$(CCMP_TEST)
	tools/wifi-run.py --console --save $(BUILD)/wifi-run.log \
		$(foreach e,$(BUILD)/wifi-drv.elf $(BUILD)/kernel-wifi-esp32c6.elf $(PHYBLOB_ROM_ELF),--symbols $(e)) -- \
		$(ESPTOOL_PYTHON) tools/esp32c6-run.py --port $(PORT) --flash $(RADIO_ESP_AT):$(BUILD)/wifi-drv.bin \
		--text $(RADIO_ESP_INPUT_AT):$(or $(WIFI_CONFIG),/dev/null) $(BUILD)/kernel-wifi-esp32c6.bin
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
WIFI_ESP_BOOT = $(ESPTOOL_PYTHON) tools/esp32c6-run.py --port $(PORT) --flash $(RADIO_ESP_AT):$(1) \
	--text $(RADIO_ESP_INPUT_AT):$(WIFI_CONFIG) $(BUILD)/kernel-wifi-esp32c6.bin
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
WIFI_ESP_OWN := $(filter $(BUILD)/$(WIFI_ESP)/% $(BUILD)/$(RADIO_ESP)/%,$(WIFI_ESP_OBJ))
.PHONY: wifi-esp32c6-refs
ifeq ($(BOARD),esp32c6)
wifi-esp32c6-refs: $(WIFI_ESP_OWN) $(BUILD)/$(RADIO_ESP)/libphy.a
	tools/esp-refs.py --nm $(NM) \
		--lib phy=$(BUILD)/$(RADIO_ESP)/libphy.a:$(PHYBLOB_CACHE)/esp32c6.rom.phy.ld $(WIFI_ESP_OWN)
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
wifi-esp32c6-attrib: $(BUILD)/wifi-drv.elf $(BUILD)/wifi-drv.bin $(PHYBLOB_ROM_ELF) $(BUILD)/$(RADIO_ESP)/libphy.a
	tools/mac-trace.py attrib --elf $(BUILD)/wifi-drv.elf --rom $(PHYBLOB_ROM_ELF) --image $(BUILD)/wifi-drv.bin \
		--lib phy=$(BUILD)/$(RADIO_ESP)/libphy.a:$(PHYBLOB_CACHE)/esp32c6.rom.phy.ld \
		$(addprefix --own ,$(WIFI_ESP_OBJ) $(LIB_OBJ)) \
		$(if $(WIFI_TRACE_ONLY),--only $(WIFI_TRACE_ONLY),) $(if $(WIFI_TRACE_SEGMENTS),--segments,) \
		$(if $(WIFI_TRACE_TOP),--top $(WIFI_TRACE_TOP),) $(WIFI_TRACE_LOG)
else
wifi-esp32c6-attrib:
	$(error the driver runs on an ESP32-C6: make BOARD=esp32c6 wifi-esp32c6-attrib)
endif

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
