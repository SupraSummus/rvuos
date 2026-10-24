# The LCD of Waveshare's ESP32-C5-LCD-1.47, user/lcd/: the program on the board, a program of PROGRAMS,
# and on the host, against the model of the LCD in user/lcd/sim/, what it shows above its bus:
# lcd-test, which make check runs, holds it to the test pattern and the model's rules,
# and lcd-sim renders what the glass shows, build/host/lcd/glass.png, and the controller's memory, memory.png,
# for a person to hold beside the board. Only lcd-sim writes images, so only it links libpng.
LCD_HOST   := build/host/lcd
LCD_SHARED := $(LCD_HOST)/app.o $(LCD_HOST)/pattern.o $(LCD_HOST)/stress.o $(LCD_HOST)/orient.o $(LCD_HOST)/font.o \
              $(LCD_HOST)/anim.o $(LCD_HOST)/noise.o $(LCD_HOST)/veins.o $(LCD_HOST)/st7789.o $(LCD_HOST)/sim/panel.o

$(LCD_HOST)/%.o: user/lcd/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -O1 -g -Wall -Wextra -Werror -Wshadow $(HOST_SAN) -MMD -MP -c $< -o $@

$(LCD_HOST)/lcd-test: $(LCD_HOST)/sim/test.o $(LCD_SHARED)
	$(HOST_CC) $(HOST_SAN) $^ -o $@

$(LCD_HOST)/lcd-sim: $(LCD_HOST)/sim/sim.o $(LCD_HOST)/sim/image.o $(LCD_SHARED)
	$(HOST_CC) $(HOST_SAN) $^ -lpng -o $@

.PHONY: lcd lcd-probe lcd-test lcd-sim

# The program on the board, its input LCD_CONFIG, words of key=value such as "scene=stress mhz=80 drive=0";
# see user/lcd/root.c. It is written into the input region on every run, so no run reads an earlier one's.
# The run ends when the program halts, or after a minute, which the orient scene's turns take most of.
LCD_CONFIG ?= scene=pattern
lcd: $(BUILD)/kernel-lcd.$(IMAGE)
ifneq ($(BOARD),esp32c5)
	$(error the LCD is Waveshare's ESP32-C5-LCD-1.47's: make BOARD=esp32c5 lcd)
endif
	printf '%s\n' "$(LCD_CONFIG)" > $(BUILD)/lcd.conf
	$(BOOT_PREFIX) --timeout 60 --text $(call board_hex,INPUT_FLASH,kernel/board/$(BOARD)/board.h):$(BUILD)/lcd.conf $<

# The probe scene, driven a pixel at a time by user/lcd/tools/probe.py, LCD_PROBE its arguments,
# such as "--corner 1 --camera http://127.0.0.1:8090/video.mjpeg".
LCD_PROBE ?= --corner 0
lcd-probe: $(BUILD)/kernel-lcd.$(IMAGE)
ifneq ($(BOARD),esp32c5)
	$(error the LCD is Waveshare's ESP32-C5-LCD-1.47's: make BOARD=esp32c5 lcd-probe)
endif
	printf '%s\n' "scene=probe" > $(BUILD)/lcd-probe.conf
	user/lcd/tools/probe.py $(LCD_PROBE) -- \
		$(BOOT_PREFIX) --text $(call board_hex,INPUT_FLASH,kernel/board/$(BOARD)/board.h):$(BUILD)/lcd-probe.conf $<
lcd-test: $(LCD_HOST)/lcd-test
	$<

# LCD_SCENE and LCD_MHZ as the board's scene= and mhz=.
LCD_SCENE ?= pattern
LCD_MHZ   ?= 40
lcd-sim: $(LCD_HOST)/lcd-sim
	$< --scene $(LCD_SCENE) --mhz $(LCD_MHZ) --memory $(LCD_HOST)/memory.png $(LCD_HOST)/glass.png

-include $(shell find $(LCD_HOST) -name '*.d' 2>/dev/null)
