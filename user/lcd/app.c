/* What the program shows; see app.h. */

#include "app.h"

#include "bus.h"
#include "pattern.h"
#include "st7789.h"

void app_run(void)
{
    st7789_init();
    st7789_fill(0, 0, LCD_W - 1, LCD_H - 1, RGB565(0, 0, 0));
    lcd_bus_backlight(1);
    pattern_draw();
}
