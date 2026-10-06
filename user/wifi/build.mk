# The Wi-Fi system on a Pico 2 W, see user/wifi/wifi.h,
# and the CYW43439's firmware beside it, which tools/cyw43-blob.py fetches and packs, never into the tree.
# The loader places the blob at the start of free RAM, FREE_RAM_BASE in kernel/board/rp2350/board.h.
# tools/wifi-run.py follows the system's log over the network while it runs, and checks it from the host.
# board_hex reads an address a board's header defines, so that what a loader writes goes where the board has it.
board_hex      = $(shell sed -n 's/^\#define $(1)[ \t].*\(0x[0-9a-fA-F]\{1,\}\).*/\1/p' $(2))
WIFI_BLOB      := build/cyw43/blob.bin
WIFI_BLOB_AT   := $(call board_hex,FREE_RAM_BASE,kernel/board/rp2350/board.h)
# What the root task is to do, lines of mode=scan|sta|ap, ssid=, pass=, bssid=, channel= and run=, seconds or 0 for good,
# from a file git does not track, such as local/wifi.conf,
# placed in its input region, INPUT_BASE in board.h; with none it scans,
# for which the input region gets an empty configuration, since the board's RAM keeps the last run's.
WIFI_CONFIG    ?=
WIFI_INPUT_AT  := $(call board_hex,INPUT_BASE,kernel/board/rp2350/board.h)

$(WIFI_BLOB): tools/cyw43-blob.py
	tools/cyw43-blob.py --cache build/cyw43 --out $@

# The IP stack of user/wifi/net.c on the host, under the sanitizers, against frames a network would send;
# and the network process's side of its clients' sockets, sock.c, with the clock's DNS and SNTP messages, sntp.c.
WIFI_TEST := build/host/wifi/net-test
SOCK_TEST := build/host/wifi/sock-test
WIFI_TEST_DEPS := user/wifi/test/frames.h user/wifi/net.c user/wifi/net.h user/wifi/bytes.h user/lib/libc.h
$(WIFI_TEST): user/wifi/test/net-test.c $(WIFI_TEST_DEPS)
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -O1 -g -Wall -Wextra -Werror -Wshadow $(HOST_SAN) -Iuser -Iuser/wifi \
		user/wifi/test/net-test.c user/wifi/net.c -o $@
$(SOCK_TEST): user/wifi/test/sock-test.c user/wifi/sock.c user/wifi/sock.h user/wifi/sntp.c user/wifi/sntp.h \
              $(WIFI_TEST_DEPS)
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -O1 -g -Wall -Wextra -Werror -Wshadow $(HOST_SAN) -Iuser -Iuser/wifi \
		user/wifi/test/sock-test.c user/wifi/sock.c user/wifi/sntp.c user/wifi/net.c -o $@

.PHONY: wifi wifi-test
wifi-test: $(WIFI_TEST) $(SOCK_TEST)
	$(WIFI_TEST)
	$(SOCK_TEST)

wifi: $(BUILD)/kernel-wifi.elf $(WIFI_BLOB)
ifneq ($(BOARD),rp2350)
	$(error the Wi-Fi system runs on a Pico 2 W: make BOARD=rp2350 wifi)
endif
	tools/wifi-run.py --save $(BUILD)/wifi-run.log --symbols $(BUILD)/kernel-wifi.elf -- \
		$(RP2350_PYTHON) tools/rp2350-run.py --ram $(WIFI_BLOB_AT):$(WIFI_BLOB) \
		--text $(WIFI_INPUT_AT):$(or $(WIFI_CONFIG),/dev/null) $(BUILD)/kernel-wifi.elf
