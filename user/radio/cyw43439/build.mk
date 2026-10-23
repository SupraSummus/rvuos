# The CYW43439's radio, see user/radio/cyw43439/README.md: our code for the chip's own processors, built for each
# at the addresses it sees, into build/cyw43/; the tools of user/radio/cyw43439/tools/ load it over the Wi-Fi system's bridge.
# Our own code for the Bluetooth core, built for it (a Cortex-M3) at the addresses it sees, which btcore/bt-map.h names;
# btcore/bt-stub.S, btcore/bt-stub.ld. The bytes go into the Wi-Fi system's image through user/wifi/bt-stub-embed.S,
# which user/wifi/bt.c uploads, with WIFI_BT=1.
ARM_CC      := clang --target=armv7m-none-eabi -mthumb -march=armv7-m
BT_STUB_ELF := build/cyw43/bt-stub.elf
build/cyw43/bt-stub.ld: user/radio/cyw43439/btcore/bt-stub.ld user/radio/cyw43439/btcore/bt-map.h
	@mkdir -p $(dir $@)
	clang -E -P -Iuser/radio/cyw43439/btcore -x c $< -o $@
$(BT_STUB_ELF): user/radio/cyw43439/btcore/bt-stub.S build/cyw43/bt-stub.ld user/radio/cyw43439/btcore/bt-map.h
	@mkdir -p $(dir $@)
	$(ARM_CC) -nostdlib -fuse-ld=lld -Wl,-T,build/cyw43/bt-stub.ld -Wl,--entry=bt_stub_start $< -o $@
build/cyw43/bt-stub.bin: $(BT_STUB_ELF)
	$(OBJCOPY) -O binary --only-section=.text $< $@
build/cyw43/bt-measure.bin: $(BT_STUB_ELF)
	$(OBJCOPY) -O binary --only-section=.measure $< $@
build/cyw43/bt-takeover.bin: $(BT_STUB_ELF)
	$(OBJCOPY) -O binary --only-section=.takeover $< $@
# The executor of user/radio/cyw43439/tools/cyw43-exec.py, for the core parked; btcore/bt-exec.c, btcore/bt-exec.ld.
build/cyw43/bt-exec.ld: user/radio/cyw43439/btcore/bt-exec.ld user/radio/cyw43439/btcore/bt-map.h
	@mkdir -p $(dir $@)
	clang -E -P -Iuser/radio/cyw43439/btcore -x c $< -o $@
build/cyw43/bt-exec.elf: user/radio/cyw43439/btcore/bt-exec.c build/cyw43/bt-exec.ld
	$(ARM_CC) -Os -ffreestanding -fno-unwind-tables -fno-asynchronous-unwind-tables -nostdlib -fuse-ld=lld \
		-Wall -Wextra -Werror -Wl,-T,build/cyw43/bt-exec.ld -Wl,--entry=exec_start $< -o $@
build/cyw43/bt-exec.bin: build/cyw43/bt-exec.elf
	$(OBJCOPY) -O binary --only-section=.text $< $@
# The receiver's samples of user/radio/cyw43439/tools/cyw43-iq.py, for the WLAN's processor; wlancore/iq-cap.c, wlancore/iq-cap.ld.
build/cyw43/iq-cap.elf: user/radio/cyw43439/wlancore/iq-cap.c user/radio/cyw43439/wlancore/iq-cap.ld
	@mkdir -p $(dir $@)
	$(ARM_CC) -O2 -ffreestanding -fno-unwind-tables -fno-asynchronous-unwind-tables -nostdlib -fuse-ld=lld \
		-Wall -Wextra -Werror -Wl,-T,user/radio/cyw43439/wlancore/iq-cap.ld -Wl,--entry=iq_start $< -o $@
build/cyw43/iq-cap.bin: build/cyw43/iq-cap.elf
	$(OBJCOPY) -O binary --only-section=.text $< $@
