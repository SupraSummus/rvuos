/* What the program shows; see app.h. */

#include "app.h"

#include "bus.h"
#include "pattern.h"
#include "st7789.h"
#include "stress.h"

static const char *const names[APP_SCENES] = { "pattern", "stress" };

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

void app_run(int scene)
{
    st7789_init();
    st7789_fill(0, 0, LCD_W - 1, LCD_H - 1, RGB565(0, 0, 0));
    lcd_bus_backlight(1);
    if (scene == APP_STRESS) {
        stress_draw();
    } else {
        pattern_draw();
    }
}
