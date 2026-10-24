#ifndef RVUOS_LCD_NOISE_H
#define RVUOS_LCD_NOISE_H

#include <stdint.h>

#include "st7789.h"

/*
 * Snow, as a television showed between stations: every pixel one of sixteen greys, eight pixels to a random word.
 * It draws pictures, each in one pass, and leaves the last on the screen, or, told 0, goes on without end.
 * On the board the words are the chip's random number generator's; see user/board/esp32c5/devices.h.
 */
#define NOISE_PICTURES 600u

void noise_draw(const struct st7789 *lcd, uint32_t (*random)(void), uint32_t pictures);

#endif
