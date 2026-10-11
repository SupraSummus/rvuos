# Thread on IEEE 802.15.4, see user/thread/root.c: so far frames between the nRF52840 and the ESP32-C6,
# which tools/thread-pair.sh runs both of.
# THREAD_CONFIG names a file of key=value lines, channel=, power=, run=, stream= and survey=, see root.c,
# which the runner leaves in the input region; with none the region gets an empty one,
# since the board's RAM keeps the last run's.
THREAD_INPUT_AT := $(call board_hex,INPUT_BASE,kernel/board/nrf52840/board.h)

.PHONY: thread
thread: $(BUILD)/kernel-thread.elf
ifneq ($(BOARD),nrf52840)
	$(error Thread runs on an nRF52840 so far: make BOARD=nrf52840 thread)
endif
	tools/nrf52840-run.py --text $(THREAD_INPUT_AT):$(or $(THREAD_CONFIG),/dev/null) $<
