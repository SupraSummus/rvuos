#ifndef RVUOS_LCD_ANIM_H
#define RVUOS_LCD_ANIM_H

#include <stdint.h>

#include "st7789.h"

/*
 * An animation that shows a tear at a glance: a white bar ANIM_BAR wide, the panel's full height, on black,
 * moving ANIM_STEP to the right each picture, each picture written in one pass from the top row to the bottom.
 * A refresh that shows part of one picture and part of the next breaks the bar across, its top and bottom apart,
 * where a camera's rolling shutter only slants it.
 * It runs ANIM_PHASES of ANIM_PICTURES pictures, out of sync with the refresh and in it in turn,
 * each phase's 0 or 1 written in its top left, and the clock, if given, times each phase into the report:
 * its whole, the gaps between pictures' starts, which in sync are a frame each while it keeps up,
 * the longest a picture took to write, and the least time left between a picture's end and the next one's start.
 */
#define ANIM_BAR      16u
#define ANIM_STEP     6u
#define ANIM_PICTURES 300u
#define ANIM_PHASES   4u

struct anim_report {
    uint64_t cycles[ANIM_PHASES];
    uint64_t shortest_gap[ANIM_PHASES], longest_gap[ANIM_PHASES];
    uint64_t longest_write[ANIM_PHASES];
    uint64_t least_slack[ANIM_PHASES];
};

void anim_draw(const struct st7789 *lcd, uint64_t (*clock)(void), struct anim_report *report);

#endif
