# Thread on IEEE 802.15.4, see user/thread/root.c: so far frames between the nRF52840 and the ESP32-C6,
# which tools/thread-pair.sh runs both of.
.PHONY: thread
thread: $(BUILD)/kernel-thread.elf
ifneq ($(BOARD),nrf52840)
	$(error Thread runs on an nRF52840 so far: make BOARD=nrf52840 thread)
endif
	tools/nrf52840-run.py $<
