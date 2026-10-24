#ifndef RVUOS_LCD_SIM_IMAGE_H
#define RVUOS_LCD_SIM_IMAGE_H

#include <stdint.h>

/*
 * Write w by h pixels, 0xRRGGBB each, row by row, as a PNG at path, each pixel a square of scale by scale.
 * Returns 0, or -1 if the file could not be written.
 */
int image_write(const char *path, const uint32_t *pixels, uint32_t w, uint32_t h, uint32_t scale);

#endif
