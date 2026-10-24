/*
 * The LCD's simulator: app.c run on the host against the model of the board's LCD, panel.c,
 * and what the glass shows written as a PNG, for a person to hold beside the board.
 *
 * Usage: lcd-sim [--scene name] [--mhz n] [--madctl n] [--backlight n] [--scale n] [--memory file.png] [--every ms dir]
 *                glass.png
 *   --scene name       the scene app.c shows, "pattern" unless given, as the board's scene=
 *   --mhz n            the bus's clock, 40 unless given, as the board's mhz=, which only the time depends on
 *   --madctl n         the orient scene's MADCTL, as the board's madctl=, each of orient.h's in turn unless given
 *   --backlight n      the backlight's level, 0 to 255, as the board's backlight=, 255 unless given
 *   --scale n          each pixel a square of n by n, 2 unless given
 *   --memory file.png  the controller's whole memory, 240 by 320, with what the glass does not show dimmed,
 *                      which shows a picture that missed the glass
 *   --every ms dir     the glass as it is each ms of simulated time, dir/frame-00000.png on, as a camera would see it
 * It writes the least the drawing took on the board, the bus's time with its waits, in the board's cycles,
 * which the board's own count, in its root task's output, compares with,
 * and fails if the model met any error, each of which it writes as it meets it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../app.h"
#include "../bus.h"
#include "panel.h"
#include "image.h"

#define BOARD_HZ 192030000u /* the ESP32-C5's core, as the kernel's clock reports it */

static uint32_t scale = 2;
static uint64_t every_ns;
static const char *frames_dir;
static uint64_t next_frame_ns;
static uint32_t frames;

/* The simulated time in the board's cycles, which the board's own clock counts. */
static uint64_t board_cycles(void)
{
    return panel_now_ns() * (BOARD_HZ / 1000u) / 1000000u;
}

/* The noise scene's source on the host, Marsaglia's xorshift32, the same snow every run. */
static uint32_t xorshift(void)
{
    static uint32_t x = 2463534242u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

static int write_glass(const char *path)
{
    static uint32_t pixels[GLASS_W * GLASS_H];
    for (uint32_t y = 0; y < GLASS_H; y++) {
        for (uint32_t x = 0; x < GLASS_W; x++) {
            pixels[y * GLASS_W + x] = panel_shown(x, y);
        }
    }
    if (image_write(path, pixels, GLASS_W, GLASS_H, scale) != 0) {
        fprintf(stderr, "lcd-sim: cannot write %s\n", path);
        return -1;
    }
    return 0;
}

static int write_memory(const char *path)
{
    static uint32_t pixels[MEMORY_W * MEMORY_H];
    for (uint32_t y = 0; y < MEMORY_H; y++) {
        for (uint32_t x = 0; x < MEMORY_W; x++) {
            int on_glass;
            uint32_t c = panel_memory(x, y, &on_glass);
            pixels[y * MEMORY_W + x] = on_glass ? c : (c >> 2) & 0x3f3f3fu;
        }
    }
    if (image_write(path, pixels, MEMORY_W, MEMORY_H, scale) != 0) {
        fprintf(stderr, "lcd-sim: cannot write %s\n", path);
        return -1;
    }
    return 0;
}

static void record(uint64_t now_ns)
{
    while (now_ns >= next_frame_ns) {
        char path[4096];
        snprintf(path, sizeof(path), "%s/frame-%05u.png", frames_dir, frames++);
        if (write_glass(path) != 0) {
            exit(1);
        }
        next_frame_ns += every_ns;
    }
}

static void usage(void)
{
    fprintf(stderr, "usage: lcd-sim [--scene name] [--mhz n] [--madctl n] [--backlight n] [--scale n] [--memory file.png] "
                    "[--every ms dir] glass.png\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *glass = 0, *memory = 0;
    int scene = APP_PATTERN;
    struct app_options options = APP_OPTIONS_DEFAULT;
    struct st7789 lcd;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--scene") == 0 && i + 1 < argc) {
            i++;
            scene = app_scene(argv[i], (uint32_t)strlen(argv[i]));
        } else if (strcmp(argv[i], "--madctl") == 0 && i + 1 < argc) {
            options.madctl = (uint32_t)strtoul(argv[++i], 0, 0);
            if (options.madctl > 0xffu) {
                usage();
            }
        } else if (strcmp(argv[i], "--backlight") == 0 && i + 1 < argc) {
            options.backlight = (uint32_t)strtoul(argv[++i], 0, 0);
            if (options.backlight > LCD_BACKLIGHT_FULL) {
                usage();
            }
        } else if (strcmp(argv[i], "--mhz") == 0 && i + 1 < argc) {
            panel_bus_mhz((uint32_t)atoi(argv[++i]));
        } else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            scale = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--memory") == 0 && i + 1 < argc) {
            memory = argv[++i];
        } else if (strcmp(argv[i], "--every") == 0 && i + 2 < argc) {
            every_ns = (uint64_t)atoi(argv[++i]) * 1000000u;
            frames_dir = argv[++i];
        } else if (argv[i][0] != '-' && glass == 0) {
            glass = argv[i];
        } else {
            usage();
        }
    }
    if (glass == 0 || scene < 0 || scale < 1 || scale > 16 || (frames_dir != 0 && every_ns == 0)) {
        usage();
    }

    lcd_bus_init();
    if (frames_dir != 0) {
        panel_on_time(record);
    }
    struct anim_report report;
    options.clock = board_cycles;
    options.report = &report;
    options.random = xorshift;
    app_run(&lcd, scene, &options);
    panel_on_time(0);
    if (scene == APP_ANIM) {
        for (uint32_t phase = 0; phase < ANIM_PHASES; phase++) {
            printf("lcd-sim: phase %u, %s: %u pictures in %llu cycles, %.2f a second\n", phase,
                   phase % 2 ? "in sync" : "not in sync", ANIM_PICTURES, (unsigned long long)report.cycles[phase],
                   (double)ANIM_PICTURES * BOARD_HZ / (double)report.cycles[phase]);
            printf("lcd-sim:   from start to start %llu to %llu cycles, a picture written in %llu at most, "
                   "%llu at least from one's end to the next one's start\n",
                   (unsigned long long)report.shortest_gap[phase], (unsigned long long)report.longest_gap[phase],
                   (unsigned long long)report.longest_write[phase], (unsigned long long)report.least_slack[phase]);
        }
    }

    uint64_t ns = panel_now_ns();
    printf("lcd-sim: drawn in %llu cycles at least, %.3f ms, %llu bytes on the bus\n",
           (unsigned long long)board_cycles(), (double)ns / 1e6, (unsigned long long)panel_bytes());
    if (frames_dir != 0) {
        printf("lcd-sim: %u frames in %s\n", frames, frames_dir);
    }
    /* A frame more, for the refresh to show the whole of what was drawn. */
    lcd_bus_sleep(ST7789_FRAME_US + 100u);
    if (write_glass(glass) != 0 || (memory != 0 && write_memory(memory) != 0)) {
        return 1;
    }
    if (panel_errors() != 0) {
        fprintf(stderr, "lcd-sim: %u errors in what the panel was sent\n", panel_errors());
        return 1;
    }
    return 0;
}
