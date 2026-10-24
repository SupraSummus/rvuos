#ifndef RVUOS_LCD_APP_H
#define RVUOS_LCD_APP_H

#include <stdint.h>

#include "anim.h"
#include "bus.h"
#include "noise.h"
#include "orient.h"
#include "st7789.h"
#include "veins.h"

/*
 * What the program shows, everything above bus.h, which the board's root task and the host's simulator both run,
 * so that the simulator renders what the board shows:
 * the controller woken, the screen cleared, the backlight lit to the options' level, and a scene.
 * The scenes, by name: "pattern", the test pattern of pattern.h, "stress", stress.h's,
 * "probe", the screen black, which the board's root task then lights a pixel at a time on, as the host asks,
 * "orient", orient.h's, with the MADCTL in options, or each of orient.h's in turn for ORIENT_ALL,
 * "timing", the screen black, which the board's root task then reads the controller's timing on,
 * "anim", anim.h's moving bar, out of sync and in it in turn, timed by options' clock into its report,
 * "noise", noise.h's snow from options' random words, or the screen black without them,
 * and "veins", veins.h's network of light, from options' random words in options' memory, or the screen black without them.
 * The bus is set up before it.
 */
enum { APP_PATTERN, APP_STRESS, APP_PROBE, APP_ORIENT, APP_TIMING, APP_ANIM, APP_NOISE, APP_VEINS, APP_SCENES };

struct app_options {
    uint32_t madctl;            /* orient.h's, or ORIENT_ALL */
    uint32_t backlight;         /* bus.h's level, LCD_BACKLIGHT_FULL unless dimmed */
    uint64_t (*clock)(void);    /* the board's cycles, for anim.h, or 0 */
    struct anim_report *report; /* anim.h's, or 0 */
    uint32_t (*random)(void);   /* a random word each call, for noise.h and veins.h, or 0 */
    uint32_t pictures;          /* noise.h's and veins.h's, 0 for no end */
    void *memory;               /* VEINS_MEMORY bytes for veins.h, aligned to four, or 0 */
    uint32_t sync;              /* veins.h's pictures in step with the refresh, else as fast as they come */
};

/* The options a scene takes when none are given. */
#define APP_OPTIONS_DEFAULT { ORIENT_ALL, LCD_BACKLIGHT_FULL, 0, 0, 0, NOISE_PICTURES, 0, 1 }

/* The scene named by the n bytes at name, or -1 for none. */
int app_scene(const char *name, uint32_t n);

/* The scene, the controller woken through lcd, which the caller keeps, as the probe goes on drawing through it. */
void app_run(struct st7789 *lcd, int scene, const struct app_options *options);

#endif
