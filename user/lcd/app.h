#ifndef RVUOS_LCD_APP_H
#define RVUOS_LCD_APP_H

/*
 * What the program shows, everything above bus.h, which the board's root task and the host's simulator both run,
 * so that the simulator renders what the board shows:
 * the controller woken, the screen cleared, the backlight lit, and the test pattern of pattern.h.
 * The bus is set up before it.
 */
void app_run(void);

#endif
