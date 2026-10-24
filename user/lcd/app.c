/* What the program shows; see app.h. */

#include "app.h"

#include "bus.h"
#include "orient.h"
#include "pattern.h"
#include "st7789.h"
#include "stress.h"

static const char *const names[APP_SCENES] = { "pattern", "stress", "probe", "orient", "timing", "anim" };

int app_scene(const char *name, uint32_t n)
{
    for (int s = 0; s < APP_SCENES; s++) {
        uint32_t i = 0;
        while (i < n && names[s][i] != '\0' && names[s][i] == name[i]) {
            i++;
        }
        if (i == n && names[s][i] == '\0') {
            return s;
        }
    }
    return -1;
}

void app_run(struct st7789 *lcd, int scene, const struct app_options *options)
{
    st7789_init(lcd);
    st7789_fill(lcd, 0, 0, LCD_W - 1, LCD_H - 1, RGB565(0, 0, 0));
    lcd_bus_backlight(options->backlight);
    if (scene == APP_STRESS) {
        stress_draw(lcd);
    } else if (scene == APP_PATTERN) {
        pattern_draw(lcd);
    } else if (scene == APP_ORIENT && options->madctl == ORIENT_ALL) {
        orient_cycle(lcd);
    } else if (scene == APP_ORIENT) {
        orient_draw(lcd, (uint8_t)options->madctl);
    } else if (scene == APP_ANIM) {
        anim_draw(lcd, options->clock, options->report);
    }
}
