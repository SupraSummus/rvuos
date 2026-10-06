# The PHY harness of user/phyblob/ on an ESP32-C6, see user/phytrace.h, around Espressif's libphy.a,
# which tools/esp-fetch.py fetches with what of ESP-IDF it needs, never into the tree.
# It is a flat image at PHYBLOB_BASE, for tools/esp32c6-run.py --ram to place beside the kernel.
PHYBLOB_CACHE := build/esp
PHYBLOB_INIT  := $(BUILD)/user/phyblob/phy_init_data.c
PHYBLOB_ROM   := $(addprefix $(PHYBLOB_CACHE)/,esp32c6.rom.ld esp32c6.rom.phy.ld esp32c6.rom.libgcc.ld esp32c6.rom.libc.ld)
PHYBLOB_OBJ   := $(BUILD)/user/phyblob/start.o $(BUILD)/user/phyblob/phyblob.o $(BUILD)/user/phyblob/phy_init_data.o
PHYBLOB       := $(BUILD)/phyblob.bin

# The init data is written last, so it stands for every file fetched.
$(PHYBLOB_INIT): tools/esp-fetch.py
	@mkdir -p $(dir $@)
	tools/esp-fetch.py --cache $(PHYBLOB_CACHE) --init-data $@

$(BUILD)/user/phyblob/phy_init_data.o: $(PHYBLOB_INIT)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/user/phyblob/%.o: ASFLAGS += -Iuser
$(BUILD)/user/phyblob/harness.ld: KERNEL_INC += -Iuser

$(BUILD)/phyblob.elf: $(PHYBLOB_OBJ) $(BUILD)/user/phyblob/harness.ld $(PHYBLOB_INIT)
	$(CC) $(LDFLAGS) -Wl,-T,$(BUILD)/user/phyblob/harness.ld $(foreach s,$(PHYBLOB_ROM),-Wl,-T,$(s)) \
		$(PHYBLOB_OBJ) $(PHYBLOB_CACHE)/libphy.a -o $@

$(PHYBLOB): $(BUILD)/phyblob.elf
	$(OBJCOPY) -O binary $< $@

.PHONY: phyblob
ifeq ($(BOARD),esp32c6)
phyblob: $(PHYBLOB)
else
phyblob:
	$(error the PHY harness runs on an ESP32-C6: make BOARD=esp32c6 phyblob)
endif

# The device registers the harness reaches, through libphy.a and the ROM's PHY functions,
# against the frames the board lists; see tools/phymap.py, which also takes --i2c and --functions.
# It reads the ROM's ELF, from Espressif's esp-rom-elfs, again never into the tree.
PHYBLOB_ROM_ELF := $(PHYBLOB_CACHE)/esp32c6_rev0_rom.elf

$(PHYBLOB_ROM_ELF): tools/esp-fetch.py
	tools/esp-fetch.py --rom-elf $@

.PHONY: phymap
ifeq ($(BOARD),esp32c6)
phymap: $(BUILD)/phyblob.elf $(PHYBLOB_ROM_ELF)
	tools/phymap.py --objdump $(OBJDUMP) $(BUILD)/phyblob.elf $(PHYBLOB_ROM_ELF) kernel/board/esp32c6/board.h
else
phymap:
	$(error the PHY harness runs on an ESP32-C6: make BOARD=esp32c6 phymap)
endif
