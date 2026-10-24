/* The moving bar; see anim.h. */

#include "anim.h"

#include "font.h"

#define BLACK RGB565(0, 0, 0)
#define WHITE RGB565(255, 255, 255)

/* The phase's digit: its top left, clear of the glass's round corner, and its dots' size. */
#define LABEL_X     24u
#define LABEL_Y     40u
#define LABEL_SCALE 4u

static void put(uint8_t *row, uint32_t x, uint16_t c)
{
    row[2 * x] = (uint8_t)(c >> 8);
    row[2 * x + 1] = (uint8_t)c;
}

/* Picture n, the bar's left edge at its place and the digit for in sync or not, written in one pass. */
static void picture(const struct st7789 *lcd, uint32_t n, uint32_t digit)
{
    _Alignas(4) uint8_t row[LCD_W * 2];
    _Alignas(4) uint8_t label[LCD_W * 2];
    uint32_t left = n * ANIM_STEP % LCD_W;
    for (uint32_t x = 0; x < LCD_W; x++) {
        put(row, x, (x + LCD_W - left) % LCD_W < ANIM_BAR ? WHITE : BLACK);
    }
    st7789_begin(lcd, 0, 0, LCD_W - 1, LCD_H - 1);
    for (uint32_t y = 0; y < LCD_H; y++) {
        if (y < LABEL_Y || y >= LABEL_Y + FONT_H * LABEL_SCALE) {
            st7789_pixels(row, sizeof(row));
            continue;
        }
        uint32_t dots = font_row(digit, (y - LABEL_Y) / LABEL_SCALE);
        for (uint32_t x = 0; x < LCD_W; x++) {
            uint32_t dot = x >= LABEL_X ? (x - LABEL_X) / LABEL_SCALE : FONT_W;
            int lit = dot < FONT_W && (dots >> (FONT_W - 1 - dot) & 1u);
            label[2 * x] = lit ? (uint8_t)(WHITE >> 8) : row[2 * x];
            label[2 * x + 1] = lit ? (uint8_t)WHITE : row[2 * x + 1];
        }
        st7789_pixels(label, sizeof(label));
    }
    st7789_end();
}

static uint64_t now(uint64_t (*clock)(void))
{
    return clock != 0 ? clock() : 0;
}

void anim_draw(const struct st7789 *lcd, uint64_t (*clock)(void), struct anim_report *report)
{
    struct anim_report seen = { 0 };
    uint32_t n = 0;
    for (uint32_t phase = 0; phase < ANIM_PHASES; phase++) {
        uint32_t sync = phase % 2;
        uint64_t start = now(clock), last_start = 0, last_end = 0;
        seen.shortest_gap[phase] = UINT64_MAX;
        seen.least_slack[phase] = UINT64_MAX;
        for (uint32_t i = 0; i < ANIM_PICTURES; i++) {
            if (sync) {
                st7789_sync();
            }
            uint64_t begun = now(clock);
            picture(lcd, n++, sync);
            uint64_t ended = now(clock);
            if (i > 0) {
                uint64_t gap = begun - last_start, slack = begun - last_end;
                seen.shortest_gap[phase] = gap < seen.shortest_gap[phase] ? gap : seen.shortest_gap[phase];
                seen.longest_gap[phase] = gap > seen.longest_gap[phase] ? gap : seen.longest_gap[phase];
                seen.least_slack[phase] = slack < seen.least_slack[phase] ? slack : seen.least_slack[phase];
            }
            if (ended - begun > seen.longest_write[phase]) {
                seen.longest_write[phase] = ended - begun;
            }
            last_start = begun;
            last_end = ended;
        }
        seen.cycles[phase] = now(clock) - start;
    }
    if (clock != 0 && report != 0) {
        *report = seen;
    }
}
