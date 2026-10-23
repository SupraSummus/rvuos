# The observer, user/observer/root.c: the nRF52840 listening for BLE's packets of an access address on one channel,
# saying how many came whole and how strong, which user/observer/le-observer.py runs and reads.
# OBSERVER_CONFIG names a file of key=value lines, channel=, aa=, crcinit=, window= and run=, see root.c,
# which the runner leaves in the input region; with none the region gets an empty one.
OBSERVER_INPUT_AT := $(call board_hex,INPUT_BASE,kernel/board/nrf52840/board.h)

.PHONY: observer
observer: $(BUILD)/kernel-observer.elf
ifneq ($(BOARD),nrf52840)
	$(error the observer runs on an nRF52840: make BOARD=nrf52840 observer)
endif
	tools/nrf52840-run.py --text $(OBSERVER_INPUT_AT):$(or $(OBSERVER_CONFIG),/dev/null) $<
