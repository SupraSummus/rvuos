/* A PNG writer over libpng, the simulator's alone; see image.h. */

#include "image.h"

#include <png.h>
#include <stdio.h>
#include <stdlib.h>

int image_write(const char *path, const uint32_t *pixels, uint32_t w, uint32_t h, uint32_t scale)
{
    png_image image = { 0 };
    image.version = PNG_IMAGE_VERSION;
    image.width = w * scale;
    image.height = h * scale;
    image.format = PNG_FORMAT_RGB;
    uint8_t *rgb = malloc((size_t)image.width * image.height * 3);
    if (rgb == 0) {
        return -1;
    }
    for (uint32_t y = 0; y < image.height; y++) {
        for (uint32_t x = 0; x < image.width; x++) {
            uint32_t c = pixels[(y / scale) * w + x / scale];
            uint8_t *p = rgb + ((size_t)y * image.width + x) * 3;
            p[0] = (uint8_t)(c >> 16);
            p[1] = (uint8_t)(c >> 8);
            p[2] = (uint8_t)c;
        }
    }
    int ok = png_image_write_to_file(&image, path, 0, rgb, 0, 0);
    free(rgb);
    return ok ? 0 : -1;
}
