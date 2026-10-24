#ifndef RVUOS_LCD_APP_H
#define RVUOS_LCD_APP_H

#include <stdint.h>

/*
 * What the program shows, everything above bus.h, which the board's root task and the host's simulator both run,
 * so that the simulator renders what the board shows:
 * the controller woken, the screen cleared, the backlight lit, and a scene.
 * The scenes, by name: "pattern", the test pattern of pattern.h, and "stress", stress.h's.
 * The bus is set up before it.
 */
enum { APP_PATTERN, APP_STRESS, APP_SCENES };

/* The scene named by the n bytes at name, or -1 for none. */
int app_scene(const char *name, uint32_t n);

void app_run(int scene);

#endif
