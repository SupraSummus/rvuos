# The ESP32-C5's radio, see user/radio/esp32c5/README.md: the programs of prog/, each an image of its own in the
# window onto flash, run by the root task of user/radio/esp32c6/, root.c, built against this board's headers.
# A program links the radio's files the two chips share, the adapter and the program's frame of user/radio/esp32c6/,
# with this directory's own, and Espressif's libraries where the radio's code is not yet its own;
# tools/esp-fetch.py --chip esp32c5 fetches them with the ROM's linker scripts, never into the tree.
RADIO_C5       := user/radio/esp32c5
RADIO_C5_CACHE := build/esp-c5
RADIO_C5_INIT  := $(BUILD)/$(RADIO_C5)/phy_init_data.c
RADIO_C5_ROM   := $(addprefix $(RADIO_C5_CACHE)/,esp32c5.rom.ld esp32c5.rom.phy.ld esp32c5.rom.libgcc.ld \
                  esp32c5.rom.libc.ld esp32c5.rom.version.ld)
RADIO_C5_ROM_ELF := $(RADIO_C5_CACHE)/esp32c5_rev100_rom.elf

# The init data is written last, so it stands for every file fetched.
$(RADIO_C5_INIT): tools/esp-fetch.py
	@mkdir -p $(dir $@)
	tools/esp-fetch.py --chip esp32c5 --cache $(RADIO_C5_CACHE) --init-data $@ --wifi

$(RADIO_C5_ROM_ELF): tools/esp-fetch.py
	tools/esp-fetch.py --chip esp32c5 --rom-elf $@

# The MAC's bring-up of the ESP32-C5, held on the host to what libpp.a's hal_init does, as the ESP32-C6's is held to
# what its libraries did on the chip: tools/hal-emu.py runs the library's function under unicorn, and wrote
# test/replay/config.txt, frozen as the ESP32-C6's is; wifi-esp32c5-replay writes it again, from the fetched library,
# once the same run of the ESP32-C6's library, given the chip's reads, has given that chip's recorded accesses value
# for value, the tool's control, but for the receive base, which their control block's place set on the chip.
# The adapter's slot 1, env_is_chip, answers 1 as the driver's does, HAL_CFG's ready bit is set, the PHY's low-rate
# call is the driver's own, and the rest answer as the host's replay test, test/mac-replay-test.c, answers the driver:
# phy_get_max_pwr from its table, slowclk_cal_get, slot 82, its period, random, slot 81, 0, and coex_pti_get from the
# coexistence library's table, osi.c's.
HAL_EMU      := $(RADIO_C5)/tools/hal-emu.py
HAL_EMU_ARGS := --osi 1=1 --osi 82=3855000 --set 0x600a4ddc=1 --stub phy_disable_low_rate \
                --table phy_get_max_pwr=1:140000000014000000141414000000001313131313131212110f0f0f000000000000000000000000000000 \
                --pti 106=0a0507070a010101010703020101010104090404090409040405050505040404040202020f0a040e000c0803010a0a0f
.PHONY: wifi-esp32c5-replay
wifi-esp32c5-replay: $(RADIO_C5_INIT) $(RADIO_C5_ROM_ELF) $(RADIO_ESP_INIT) $(PHYBLOB_ROM_ELF)
	$(HAL_EMU) --chip esp32c6 --cache $(PHYBLOB_CACHE) --rom $(PHYBLOB_ROM_ELF) $(HAL_EMU_ARGS)0f \
		--reads $(RADIO_ESP)/test/replay/config.txt hal_init | grep -E '^[RW]' > $(BUILD)/hal-emu-esp32c6.txt
	grep -E '^[RW]' $(RADIO_ESP)/test/replay/config.txt | grep -v '^W4 0x600a4084 ' \
		| diff - $(BUILD)/hal-emu-esp32c6.txt --ignore-matching-lines='^W4 0x600a4084 ' \
		|| { echo "hal-emu: the ESP32-C6's run is not the chip's" >&2; exit 1; }
	@echo "hal-emu: the ESP32-C6's run is the chip's, value for value, but for the receive base"
	{ echo "# hal_init of libpp.a for the ESP32-C5, run by tools/hal-emu.py $(HAL_EMU_ARGS)"; \
	  $(HAL_EMU) --chip esp32c5 --cache $(RADIO_C5_CACHE) --rom $(RADIO_C5_ROM_ELF) $(HAL_EMU_ARGS) hal_init \
	  | grep -E '^[RW]'; } > $(RADIO_C5)/test/replay/config.txt

# The MAC's bring-up of macstart.c built for the ESP32-C5 and held on the host to that file, as the ESP32-C6's is,
# by the same test, test/mac-replay-test.c of user/radio/esp32c6/; make check runs it, which fetches nothing.
RADIO_C5_HOST          := build/host/esp32c5
MAC_REPLAY_TEST_C5     := $(RADIO_C5_HOST)/mac-replay-test
MAC_REPLAY_TEST_C5_OBJ := $(addprefix $(RADIO_C5_HOST)/,macstart.o btbb.o btbb-host.o mac-replay-test.o)
$(RADIO_C5_HOST)/%.o: $(RADIO_ESP)/%.c $(RADIO_ESP)/mac.h $(RADIO_ESP)/macregs.h user/board/esp32c5/radio.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wshadow $(HOST_SAN) -DMAC_HOST -Iuser/board/esp32c5 \
		-I$(RADIO_ESP) -Iuser -c $< -o $@
$(RADIO_C5_HOST)/%.o: $(RADIO_ESP)/test/%.c $(RADIO_ESP)/mac.h $(RADIO_ESP)/macregs.h user/board/esp32c5/radio.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wshadow $(HOST_SAN) -DMAC_HOST -Iuser/board/esp32c5 \
		-I$(RADIO_ESP) -Iuser -c $< -o $@
$(MAC_REPLAY_TEST_C5): $(MAC_REPLAY_TEST_C5_OBJ)
	$(HOST_CC) $(HOST_SAN) $^ -o $@
.PHONY: mac-replay-test-c5
mac-replay-test-c5: $(MAC_REPLAY_TEST_C5)
	$(MAC_REPLAY_TEST_C5) $(RADIO_C5)/test/replay config $(RADIO_C5)/test/replay-expected.txt

ifeq ($(BOARD),esp32c5)
# The files the two chips' programs share, the program's frame, the adapter, its heap and the logs' text,
# this directory's own, and the tracer's decoder and served accesses.
RADIO_C5_SHARED := $(addprefix $(BUILD)/$(RADIO_ESP)/,radio.o osi.o heap.o glue.o mac.o macstart.o txctl.o)
RADIO_C5_OBJ    := $(patsubst %.c,$(BUILD)/%.o,$(wildcard $(RADIO_C5)/*.c)) $(BUILD)/$(RADIO_C5)/phy_init_data.o \
                   $(RADIO_C5_SHARED) $(BUILD)/user/tracer/trace.o $(BUILD)/user/tracer/watch.o
RADIO_C5_LIBS   := $(RADIO_C5_CACHE)/libphy.a
RADIO_C5_PROGS  := $(basename $(notdir $(wildcard $(RADIO_C5)/prog/*.c)))
$(BUILD)/$(RADIO_C5)/%.o: CFLAGS += -I$(RADIO_ESP) -I$(RADIO_C5)
$(BUILD)/$(RADIO_C5)/%.o: $(RADIO_C5_INIT)

$(BUILD)/$(RADIO_C5)/phy_init_data.o: $(RADIO_C5_INIT)
	$(CC) $(CFLAGS) -c $< -o $@

# A program in the free RAM's upper half, as on the ESP32-C6 a program lies in a smaller block than the station's.
$(BUILD)/$(RADIO_C5)/flash-prog.ld: KERNEL_INC += -Iuser/board/$(BOARD)
$(BUILD)/$(RADIO_C5)/flash-prog.ld: $(RADIO_ESP)/flash.ld.S
	@mkdir -p $(dir $@)
	$(CC) $(ARCHFLAGS) -E -P -x assembler-with-cpp $(KERNEL_INC) -Iinclude -DDRV_RAM_BASE=0x40850000 \
		-DDRV_RAM_SIZE=0x00010000 -MMD -MP -MT $@ $< -o $@

$(BUILD)/radio-%.elf: $(BUILD)/$(RADIO_C5)/prog/%.o $(RADIO_C5_OBJ) $(LIB_OBJ) $(BUILD)/$(RADIO_C5)/flash-prog.ld $(RADIO_C5_INIT)
	$(CC) $(LDFLAGS) -Wl,-T,$(BUILD)/$(RADIO_C5)/flash-prog.ld $(foreach s,$(RADIO_C5_ROM),-Wl,-T,$(s)) \
		$< $(RADIO_C5_OBJ) $(LIB_OBJ) -Wl,--start-group $(RADIO_C5_LIBS) -Wl,--end-group -o $@

# A program's run, as on the ESP32-C6: the root task's image, the program written at the window onto flash if it
# changed, and its configuration, RADIO_CONFIG, in the input region; the log stays in build/esp32c5/radio-run.log.
.PHONY: radio
radio: $(BUILD)/kernel-wifi-esp32c5.bin $(if $(PROG),$(BUILD)/radio-$(PROG).bin) $(RADIO_C5_ROM_ELF)
	$(if $(filter $(PROG),$(RADIO_C5_PROGS)),,$(error name a program of $(RADIO_C5)/prog/: PROG=$(RADIO_C5_PROGS)))
	tools/wifi-run.py --console --save $(BUILD)/radio-run.log \
		$(foreach e,$(BUILD)/radio-$(PROG).elf $(BUILD)/kernel-wifi-esp32c5.elf $(RADIO_C5_ROM_ELF),--symbols $(e)) -- \
		$(ESPTOOL_PYTHON) tools/esp32c6-run.py --chip esp32c5 --port $(PORT) \
		--flash $(call board_hex,FLASH_WINDOW_FLASH,user/board/esp32c5/devices.h):$(BUILD)/radio-$(PROG).bin \
		--text $(call board_hex,INPUT_FLASH,kernel/board/esp32c5/board.h):$(or $(RADIO_CONFIG),/dev/null) \
		$(BUILD)/kernel-wifi-esp32c5.bin
endif
