# The LCD of Waveshare's ESP32-C5-LCD-1.47, user/lcd/: the program on the board, a program of PROGRAMS,
# and on the host, against the model of the LCD in user/lcd/sim/, what it shows above its bus:
# lcd-test, which make check runs, holds it to the test pattern and the model's rules,
# and lcd-sim renders what the glass shows, build/host/lcd/glass.png, and the controller's memory, memory.png,
# for a person to hold beside the board. Only lcd-sim writes images, so only it links libpng.
LCD_HOST   := build/host/lcd
LCD_SHARED := $(LCD_HOST)/app.o $(LCD_HOST)/pattern.o $(LCD_HOST)/st7789.o $(LCD_HOST)/sim/panel.o

$(LCD_HOST)/%.o: user/lcd/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -O1 -g -Wall -Wextra -Werror -Wshadow $(HOST_SAN) -MMD -MP -c $< -o $@

$(LCD_HOST)/lcd-test: $(LCD_HOST)/sim/test.o $(LCD_SHARED)
	$(HOST_CC) $(HOST_SAN) $^ -o $@

$(LCD_HOST)/lcd-sim: $(LCD_HOST)/sim/sim.o $(LCD_HOST)/sim/image.o $(LCD_SHARED)
	$(HOST_CC) $(HOST_SAN) $^ -lpng -o $@

.PHONY: lcd-test lcd-sim
lcd-test: $(LCD_HOST)/lcd-test
	$<

lcd-sim: $(LCD_HOST)/lcd-sim
	$< --memory $(LCD_HOST)/memory.png $(LCD_HOST)/glass.png

-include $(shell find $(LCD_HOST) -name '*.d' 2>/dev/null)
